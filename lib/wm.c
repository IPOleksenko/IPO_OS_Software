#include <wm.h>
#include <syscall.h>
#include <string.h>
#include <vga.h>

/* =========================================================================
 * 6x8 bitmap font (printable ASCII 0x20..0x7E)
 * ========================================================================= */
static const uint8_t *wm_font_data = (const uint8_t *)(
/*sp*/"\x00\x00\x00\x00\x00\x00\x00\x00"
/* ! */"\x20\x20\x20\x20\x00\x20\x00\x00"
/* \" */"\x50\x50\x00\x00\x00\x00\x00\x00"
/* # */"\x50\xF8\x50\x50\xF8\x50\x00\x00"
/* $ */"\x20\x78\xA0\x70\x28\xF0\x20\x00"
/* % */"\xC0\xC8\x10\x20\x40\x98\x18\x00"
/* & */"\x40\xA0\xA0\x40\xA8\x90\x68\x00"
/* ' */"\x20\x40\x00\x00\x00\x00\x00\x00"
/* ( */"\x10\x20\x40\x40\x40\x20\x10\x00"
/* ) */"\x40\x20\x10\x10\x10\x20\x40\x00"
/* * */"\x00\x20\xA8\x70\xA8\x20\x00\x00"
/* + */"\x00\x20\x20\xF8\x20\x20\x00\x00"
/* , */"\x00\x00\x00\x00\x00\x20\x40\x00"
/* - */"\x00\x00\x00\xF8\x00\x00\x00\x00"
/* . */"\x00\x00\x00\x00\x00\x60\x60\x00"
/* / */"\x08\x10\x10\x20\x40\x40\x80\x00"
/* 0 */"\x70\x88\x88\x88\x88\x88\x70\x00"
/* 1 */"\x20\x60\x20\x20\x20\x20\x70\x00"
/* 2 */"\x70\x88\x08\x30\x40\x80\xF8\x00"
/* 3 */"\xF8\x08\x10\x30\x08\x88\x70\x00"
/* 4 */"\x10\x30\x50\x90\xF8\x10\x10\x00"
/* 5 */"\xF8\x80\xF0\x08\x08\x88\x70\x00"
/* 6 */"\x38\x40\x80\xF0\x88\x88\x70\x00"
/* 7 */"\xF8\x08\x10\x20\x20\x40\x40\x00"
/* 8 */"\x70\x88\x88\x70\x88\x88\x70\x00"
/* 9 */"\x70\x88\x88\x78\x08\x10\xE0\x00"
/* : */"\x00\x60\x60\x00\x60\x60\x00\x00"
/* ; */"\x00\x20\x00\x20\x20\x40\x00\x00"
/* < */"\x10\x20\x40\x80\x40\x20\x10\x00"
/* = */"\x00\x00\xF8\x00\xF8\x00\x00\x00"
/* > */"\x80\x40\x20\x10\x20\x40\x80\x00"
/* ? */"\x70\x88\x10\x20\x20\x00\x20\x00"
/* @ */"\x70\x88\xA8\xB8\xB0\x80\x70\x00"
/* A */"\x20\x50\x88\x88\xF8\x88\x88\x00"
/* B */"\xF0\x88\x88\xF0\x88\x88\xF0\x00"
/* C */"\x70\x88\x80\x80\x80\x88\x70\x00"
/* D */"\xE0\x90\x88\x88\x88\x90\xE0\x00"
/* E */"\xF8\x80\x80\xF0\x80\x80\xF8\x00"
/* F */"\xF8\x80\x80\xF0\x80\x80\x80\x00"
/* G */"\x70\x88\x80\xB8\x88\x88\x70\x00"
/* H */"\x88\x88\x88\xF8\x88\x88\x88\x00"
/* I */"\x70\x20\x20\x20\x20\x20\x70\x00"
/* J */"\x38\x10\x10\x10\x10\x90\x60\x00"
/* K */"\x88\x90\xA0\xC0\xA0\x90\x88\x00"
/* L */"\x80\x80\x80\x80\x80\x80\xF8\x00"
/* M */"\x88\xD8\xA8\xA8\x88\x88\x88\x00"
/* N */"\x88\xC8\xA8\x98\x88\x88\x88\x00"
/* O */"\x70\x88\x88\x88\x88\x88\x70\x00"
/* P */"\xF0\x88\x88\xF0\x80\x80\x80\x00"
/* Q */"\x70\x88\x88\x88\xA8\x90\x68\x00"
/* R */"\xF0\x88\x88\xF0\xA0\x90\x88\x00"
/* S */"\x70\x88\x80\x70\x08\x88\x70\x00"
/* T */"\xF8\x20\x20\x20\x20\x20\x20\x00"
/* U */"\x88\x88\x88\x88\x88\x88\x70\x00"
/* V */"\x88\x88\x88\x50\x50\x20\x20\x00"
/* W */"\x88\x88\x88\xA8\xA8\xD8\x88\x00"
/* X */"\x88\x88\x50\x20\x50\x88\x88\x00"
/* Y */"\x88\x88\x50\x20\x20\x20\x20\x00"
/* Z */"\xF8\x08\x10\x20\x40\x80\xF8\x00"
/* [ */"\x70\x40\x40\x40\x40\x40\x70\x00"
/* \\ */"\x80\x40\x40\x20\x10\x10\x08\x00"
/* ] */"\x70\x10\x10\x10\x10\x10\x70\x00"
/* ^ */"\x20\x50\x88\x00\x00\x00\x00\x00"
/* _ */"\x00\x00\x00\x00\x00\x00\xF8\x00"
/* ` */"\x40\x20\x00\x00\x00\x00\x00\x00"
/* a */"\x00\x00\x70\x08\x78\x88\x78\x00"
/* b */"\x80\x80\xF0\x88\x88\x88\xF0\x00"
/* c */"\x00\x00\x70\x88\x80\x88\x70\x00"
/* d */"\x08\x08\x78\x88\x88\x88\x78\x00"
/* e */"\x00\x00\x70\x88\xF8\x80\x70\x00"
/* f */"\x30\x48\x40\xF0\x40\x40\x40\x00"
/* g */"\x00\x00\x78\x88\x78\x08\x70\x00"
/* h */"\x80\x80\xF0\x88\x88\x88\x88\x00"
/* i */"\x20\x00\x60\x20\x20\x20\x70\x00"
/* j */"\x10\x00\x30\x10\x10\x90\x60\x00"
/* k */"\x80\x80\x90\xA0\xC0\xA0\x90\x00"
/* l */"\x60\x20\x20\x20\x20\x20\x70\x00"
/* m */"\x00\x00\xD8\xA8\xA8\xA8\xA8\x00"
/* n */"\x00\x00\xF0\x88\x88\x88\x88\x00"
/* o */"\x00\x00\x70\x88\x88\x88\x70\x00"
/* p */"\x00\x00\xF0\x88\x88\xF0\x80\x00"
/* q */"\x00\x00\x78\x88\x88\x78\x08\x00"
/* r */"\x00\x00\xB0\xC8\x80\x80\x80\x00"
/* s */"\x00\x00\x78\x80\x70\x08\xF0\x00"
/* t */"\x40\x40\xF0\x40\x40\x48\x30\x00"
/* u */"\x00\x00\x88\x88\x88\x88\x78\x00"
/* v */"\x00\x00\x88\x88\x50\x50\x20\x00"
/* w */"\x00\x00\x88\x88\xA8\xA8\x50\x00"
/* x */"\x00\x00\x88\x50\x20\x50\x88\x00"
/* y */"\x00\x00\x88\x88\x78\x08\x70\x00"
/* z */"\x00\x00\xF8\x10\x20\x40\xF8\x00"
/* { */"\x18\x20\x20\x60\x20\x20\x18\x00"
/* | */"\x20\x20\x20\x00\x20\x20\x20\x00"
/* } */"\xC0\x20\x20\x30\x20\x20\xC0\x00"
/* ~ */"\x48\x90\x00\x00\x00\x00\x00\x00"
);

static int wm_call_window(uint32_t syscall_num, wm_window_t *win) {
    if (!win) return 0;
    uint32_t args[1] = { (uint32_t)(uintptr_t)win };
    return ipo_syscall(syscall_num, 1u, args);
}

wm_window_t *wm_create_window(const wm_window_options_t *opts) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)opts;
    return (wm_window_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_WM_CREATE_WINDOW, 1u, args);
}

void wm_destroy_window(wm_window_t *win) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_DESTROY_WINDOW, 1u, args);
}

bool wm_is_window_valid(wm_window_t *win) {
    if (!win) return false;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    return (bool)ipo_syscall(IPO_SYSCALL_WM_IS_VALID, 1u, args);
}

void wm_session_start(void) {
    ipo_syscall(IPO_SYSCALL_WM_SESSION_START, 0u, NULL);
}

void wm_session_stop(void) {
    ipo_syscall(IPO_SYSCALL_WM_SESSION_STOP, 0u, NULL);
}

bool wm_session_active(void) {
    return (bool)ipo_syscall(IPO_SYSCALL_WM_SESSION_ACTIVE, 0u, NULL);
}

int wm_get_window_count(void) {
    return (int)ipo_syscall(IPO_SYSCALL_WM_GET_COUNT, 0u, NULL);
}

int wm_get_window_count_for_pid(uint32_t pid) {
    uint32_t args[1] = { pid };
    return ipo_syscall(IPO_SYSCALL_WM_GET_COUNT_PID, 1u, args);
}

bool wm_has_windows_for_pid(uint32_t pid) {
    uint32_t args[1] = { pid };
    return ipo_syscall(IPO_SYSCALL_WM_HAS_WINDOWS_PID, 1u, args) != 0;
}

wm_window_t *wm_get_focused(void) {
    return (wm_window_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_WM_GET_FOCUSED, 0u, NULL);
}

wm_window_t *wm_get_window_list(void) {
    return (wm_window_t *)(uintptr_t)ipo_syscall(IPO_SYSCALL_WM_GET_LIST, 0u, NULL);
}

void wm_set_focus(wm_window_t *win) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_SET_FOCUS, 1u, args);
}

void wm_focus_window_for_pid(uint32_t pid) {
    uint32_t args[1] = { pid };
    ipo_syscall(IPO_SYSCALL_WM_FOCUS_PID, 1u, args);
}

void wm_focus_next(void) {
    ipo_syscall(IPO_SYSCALL_WM_FOCUS_NEXT, 0u, NULL);
}

void wm_focus_prev(void) {
    ipo_syscall(IPO_SYSCALL_WM_FOCUS_PREV, 0u, NULL);
}

void wm_get_screen_bounds(int *out_w, int *out_h) {
    vga_gfx_get_screen_bounds(out_w, out_h);
}

void wm_invalidate(wm_window_t *win) {
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_INVALIDATE, 1u, args);
}

void wm_invalidate_all(void) {
    ipo_syscall(IPO_SYSCALL_WM_INVALIDATE_ALL, 0u, NULL);
}

void wm_init(void) {
    ipo_syscall(IPO_SYSCALL_WM_INIT, 0u, NULL);
}

void wm_set_fullscreen(wm_window_t *win, bool enable) {
    if (!win) return;
    uint32_t args[2] = { (uint32_t)(uintptr_t)win, (uint32_t)enable };
    ipo_syscall(IPO_SYSCALL_WM_SET_FULLSCREEN, 2u, args);
}

void wm_minimize(wm_window_t *win) {
    wm_call_window(IPO_SYSCALL_WM_MINIMIZE, win);
}

void wm_restore(wm_window_t *win) {
    wm_call_window(IPO_SYSCALL_WM_RESTORE, win);
}

void wm_toggle_maximize(wm_window_t *win) {
    if (!win) return;
    uint32_t args[1];
    args[0] = (uint32_t)(uintptr_t)win;
    ipo_syscall(IPO_SYSCALL_WM_TOGGLE_MAXIMIZE, 1u, args);
}

void wm_move(wm_window_t *win, int16_t nx, int16_t ny) {
    if (!win) return;
    uint32_t args[3] = { (uint32_t)(uintptr_t)win, (uint32_t)(int32_t)nx,
                         (uint32_t)(int32_t)ny };
    ipo_syscall(IPO_SYSCALL_WM_MOVE, 3u, args);
}

bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh) {
    if (!win) return false;
    uint32_t args[3];
    args[0] = (uint32_t)(uintptr_t)win;
    args[1] = (uint32_t)nw;
    args[2] = (uint32_t)nh;
    return (bool)ipo_syscall(IPO_SYSCALL_WM_RESIZE, 3u, args);
}

void wm_set_title(wm_window_t *win, const char *title) {
    if (!win) return;
    uint32_t args[2] = { (uint32_t)(uintptr_t)win, (uint32_t)(uintptr_t)title };
    ipo_syscall(IPO_SYSCALL_WM_SET_TITLE, 2u, args);
}

void wm_set_shape(wm_window_t *win, const uint8_t *mask, uint16_t mw, uint16_t mh) {
    if (!win) return;
    uint32_t args[4] = { (uint32_t)(uintptr_t)win, (uint32_t)(uintptr_t)mask,
                         (uint32_t)mw, (uint32_t)mh };
    ipo_syscall(IPO_SYSCALL_WM_SET_SHAPE, 4u, args);
}

void wm_set_cursor_visible(wm_window_t *win, bool visible) {
    if (!win) return;
    uint32_t args[2] = { (uint32_t)(uintptr_t)win, visible ? 1u : 0u };
    ipo_syscall(IPO_SYSCALL_WM_SET_CURSOR_VISIBLE, 2u, args);
}

bool wm_get_cursor_visible(const wm_window_t *win) {
    if (!win) return true;
    uint32_t args[1] = { (uint32_t)(uintptr_t)win };
    return ipo_syscall(IPO_SYSCALL_WM_GET_CURSOR_VISIBLE, 1u, args) != 0;
}

bool wm_poll_event(wm_window_t *win, wm_event_t *out) {
    if (!win || !out) return false;
    uint32_t args[2] = { (uint32_t)(uintptr_t)win, (uint32_t)(uintptr_t)out };
    return ipo_syscall(IPO_SYSCALL_WM_POLL_EVENT, 2u, args) != 0u;
}

void wm_compositor_tick(void) {
    ipo_syscall(IPO_SYSCALL_WM_COMPOSITOR_TICK, 0u, NULL);
}

/* Userland buffer drawing helpers */
void wm_buf_put_pixel(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t c) {
    if (x >= 0 && x < (int)bw && y >= 0 && y < (int)bh && buf) {
        buf[y * bw + x] = c;
    }
}

void wm_buf_fill_rect(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t c) {
    if (!buf || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    int x2 = x + w;
    int y2 = y + h;
    if (x2 > (int)bw) x2 = (int)bw;
    if (y2 > (int)bh) y2 = (int)bh;
    if (x2 <= x || y2 <= y) return;
    for (int cy = y; cy < y2; cy++) {
        memset(buf + (size_t)cy * bw + x, c, (size_t)(x2 - x));
    }
}

void wm_buf_draw_rect(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t c) {
    if (!buf || w <= 0 || h <= 0) return;
    for (int cx = x; cx < x + w; cx++) {
        wm_buf_put_pixel(buf, bw, bh, cx, y, c);
        wm_buf_put_pixel(buf, bw, bh, cx, y + h - 1, c);
    }
    for (int cy = y; cy < y + h; cy++) {
        wm_buf_put_pixel(buf, bw, bh, x, cy, c);
        wm_buf_put_pixel(buf, bw, bh, x + w - 1, cy, c);
    }
}

void wm_buf_draw_line(uint8_t *buf, uint16_t bw, uint16_t bh, int x0, int y0, int x1, int y1, uint8_t c) {
    if (!buf) return;
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    while (1) {
        wm_buf_put_pixel(buf, bw, bh, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}


void wm_buf_draw_glyph(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t slot, uint8_t c) {
    if (!buf) return;
    if (slot >= 0x20 && slot <= 0x7E) {
        /* Fast path: built-in crisp 6x8 ASCII bitmap */
        const uint8_t *g = wm_font_data + (size_t)(slot - 0x20) * WM_FONT_H;
        for (int row = 0; row < WM_FONT_H; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < WM_FONT_W; col++) {
                if (bits & (0x80u >> col)) {
                    wm_buf_put_pixel(buf, bw, bh, x + col, y + row, c);
                }
            }
        }
    } else {
        /* Extended glyph (e.g. Cyrillic): downsample 8x16 → 6x8 */
        const uint8_t *bmp = vga_font_get_cached_glyph(slot);
        if (!bmp) return;
        for (int row = 0; row < WM_FONT_H; row++) {
            uint8_t bits = bmp[row * 2] | bmp[row * 2 + 1];
            for (int col = 0; col < WM_FONT_W; col++) {
                if (bits & (0x40u >> col)) {
                    wm_buf_put_pixel(buf, bw, bh, x + col, y + row, c);
                }
            }
        }
    }
}

void wm_buf_draw_glyph_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t slot, uint8_t fg, uint8_t bg) {
    if (!buf) return;
    if (slot >= 0x20 && slot <= 0x7E) {
        const uint8_t *g = wm_font_data + (size_t)(slot - 0x20) * WM_FONT_H;
        for (int row = 0; row < WM_FONT_H; row++) {
            uint8_t bits = g[row];
            for (int col = 0; col < WM_FONT_W; col++) {
                uint8_t colr = (bits & (0x80u >> col)) ? fg : bg;
                wm_buf_put_pixel(buf, bw, bh, x + col, y + row, colr);
            }
        }
    } else {
        const uint8_t *bmp = vga_font_get_cached_glyph(slot);
        if (!bmp) {
            wm_buf_fill_rect(buf, bw, bh, x, y, WM_FONT_W, WM_FONT_H, bg);
            return;
        }
        for (int row = 0; row < WM_FONT_H; row++) {
            uint8_t bits = bmp[row * 2] | bmp[row * 2 + 1];
            for (int col = 0; col < WM_FONT_W; col++) {
                uint8_t colr = (bits & (0x40u >> col)) ? fg : bg;
                wm_buf_put_pixel(buf, bw, bh, x + col, y + row, colr);
            }
        }
    }
}

void wm_buf_draw_char(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char ch, uint8_t c) {
    uint8_t u = (uint8_t)ch;
    wm_buf_draw_glyph(buf, bw, bh, x, y, u, c);
}

void wm_buf_draw_char_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char ch, uint8_t fg, uint8_t bg) {
    uint8_t u = (uint8_t)ch;
    wm_buf_draw_glyph_bg(buf, bw, bh, x, y, u, fg, bg);
}

void wm_buf_draw_string(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t c) {
    if (!str || !buf) return;
    int cx = x;
    size_t len = strlen(str);
    size_t i = 0;
    while (i < len) {
        if (str[i] == '\n') {
            cx = x;
            y += WM_FONT_H;
            i++;
            continue;
        }
        if (str[i] == '\t') {
            int tab_step = WM_FONT_W * 4;
            int rel = cx - x;
            cx = x + ((rel / tab_step) + 1) * tab_step;
            i++;
            continue;
        }
        size_t bytes = 0;
        uint8_t slot = utf8_to_vga_glyph(&str[i], len - i, &bytes);
        if (bytes == 0) {
            i++;
            continue;
        }
        wm_buf_draw_glyph(buf, bw, bh, cx, y, slot, c);
        cx += WM_FONT_W;
        i += bytes;
    }
}

void wm_buf_draw_string_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t fg, uint8_t bg) {
    if (!str || !buf) return;
    int cx = x;
    size_t len = strlen(str);
    size_t i = 0;
    while (i < len) {
        if (str[i] == '\n') {
            cx = x;
            y += WM_FONT_H;
            i++;
            continue;
        }
        if (str[i] == '\t') {
            int tab_step = WM_FONT_W * 4;
            int rel = cx - x;
            int next_cx = x + ((rel / tab_step) + 1) * tab_step;
            wm_buf_fill_rect(buf, bw, bh, cx, y, next_cx - cx, WM_FONT_H, bg);
            cx = next_cx;
            i++;
            continue;
        }
        size_t bytes = 0;
        uint8_t slot = utf8_to_vga_glyph(&str[i], len - i, &bytes);
        if (bytes == 0) {
            i++;
            continue;
        }
        wm_buf_draw_glyph_bg(buf, bw, bh, cx, y, slot, fg, bg);
        cx += WM_FONT_W;
        i += bytes;
    }
}
