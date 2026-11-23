/**
 * wm.h  —  Window Manager API for IPO_OS (VGA Mode 13h: 320×200, 256 colours)
 *
 * Architecture
 * ============
 * The WM runs as a persistent kernel async task — not inside any single app.
 *
 * wm_session_start()  — enters VGA mode, allocates backbuffer, registers the
 *                        compositor as an async task, returns immediately.
 * wm_session_stop()   — stops the async task, returns to text mode.
 *
 * Apps that want to show windows:
 *   1. Call wm_create_window() → window added to
 *      the shared global list.
 *   2. Return from main().  The compositor task keeps drawing their window.
 *   3. Optionally call wm_destroy_window() before returning to remove it.
 *
 * Multiple separate programs running simultaneously can each create windows
 * independently:
 *   - each program creates its windows and returns or runs an event loop.
 *   - The WM async task composites all active window sets every tick.
 *
 * System overlay
 * ==============
 * The compositor always draws a small [✕] button in the bottom-left corner.
 * Clicking it (left mouse button) calls wm_session_stop().
 *
 * Mouse cursor
 * ============
 * The compositor draws a software arrow cursor on every frame using the
 * position from mouse_get_state(). Mouse boundaries are dynamically derived
 * from the active display area during session start/mode switches.
 */

#ifndef LIB_WM_H
#define LIB_WM_H

#include <stdint.h>
#include <stdbool.h>
#include <vga_gfx.h>

/* =========================================================================
 * Decoration style
 * ========================================================================= */
typedef enum {
    WM_DECOR_DEFAULT = 0, /* Standard title bar + border                     */
    WM_DECOR_THIN,        /* 1-pixel border, no title bar                    */
    WM_DECOR_NONE,        /* No border or title bar at all                   */
} wm_decor_style_t;

#define WM_FONT_W 6
#define WM_FONT_H 8

/* =========================================================================
 * Event codes passed to event_cb
 * ========================================================================= */
#define WM_EVENT_KEY_DOWN   0x00000001u
#define WM_EVENT_KEY_UP     0x00000002u
#define WM_EVENT_KEY_EXT    0x00000004u  /* full 32-bit keycode in low 31 bits; bit 31 = break */
#define WM_EVENT_FOCUS_IN   0x00000010u
#define WM_EVENT_FOCUS_OUT  0x00000020u
#define WM_EVENT_CLOSE      0x00000100u
#define WM_EVENT_RESIZE     0x00000200u
#define WM_EVENT_MOVE       0x00000400u
#define WM_EVENT_CLICK            0x00001000u  /* left mouse button down inside window */
#define WM_EVENT_SHORTCUT         0x00002000u  /* custom registered shortcut fired (data = tag) */
#define WM_EVENT_MOUSE_MOVE       0x00004000u  /* high 16 bits = x, low 16 bits = y (matches WM_EVENT_CLICK) */
#define WM_EVENT_MOUSE_UP         0x00008000u  /* left mouse button up inside window */
#define WM_EVENT_MOUSE_RIGHT_DOWN 0x00010000u
#define WM_EVENT_MOUSE_RIGHT_UP   0x00020000u
#define WM_EVENT_MOUSE_MIDDLE_DOWN 0x00040000u
#define WM_EVENT_MOUSE_MIDDLE_UP  0x00080000u
#define WM_EVENT_MOUSE_WHEEL       0x00100000u  /* data = (uint32_t)(int32_t)dz */

/* =========================================================================
 * Forward declaration
 * ========================================================================= */
typedef struct wm_window wm_window_t;

/* =========================================================================
 * Callback signatures
 * ========================================================================= */
/* Polled event record returned by wm_poll_event() */
typedef struct {
    uint32_t type;
    uint32_t data;
} wm_event_t;

/* =========================================================================
 * Window options – pass to wm_create_window()
 * ========================================================================= */
typedef struct {
    int16_t  x, y;
    uint16_t w, h;
    bool     fullscreen;

    const char *title;

    wm_decor_style_t decor_style;
    uint8_t  bg_color;
    uint8_t  border_color;
    uint8_t  title_bg;
    uint8_t  title_fg;

    /* Shape mask (NULL = rectangular) */
    const uint8_t *shape_mask;
    uint16_t mask_w, mask_h;

    /* NULL = auto-allocate */
    uint8_t *framebuf;

    void         *user_data;
} wm_window_options_t;

#define WM_WINDOW_OPTIONS_DEFAULT {   \
    .x = 10, .y = 10,                 \
    .w = 100, .h = 60,                \
    .fullscreen  = false,             \
    .title       = NULL,              \
    .decor_style = WM_DECOR_DEFAULT,  \
    .bg_color    = 7,                 \
    .border_color= 15,                \
    .title_bg    = 1,                 \
    .title_fg    = 15,                \
    .shape_mask  = NULL,              \
    .mask_w = 0, .mask_h = 0,         \
    .framebuf    = NULL,              \
    .user_data   = NULL,              \
}

/* =========================================================================
 * Window structure
 * ========================================================================= */
struct wm_window {
    int16_t  x, y;
    uint16_t w, h;
    bool     fullscreen;
    bool     minimized;
    bool     maximized;
    int16_t  saved_x, saved_y;
    uint16_t saved_w, saved_h;
    int16_t  fullscreen_saved_x, fullscreen_saved_y;
    uint16_t fullscreen_saved_w, fullscreen_saved_h;

    char *title;

    wm_decor_style_t decor_style;
    uint8_t  bg_color;
    uint8_t  border_color;
    uint8_t  title_bg;
    uint8_t  title_fg;

    uint8_t *framebuf;
    bool     framebuf_owned;

    const uint8_t *shape_mask;
    uint16_t mask_w, mask_h;
    bool     shape_mask_owned;  /* true if the WM owns a copied shape mask */

    void         *user_data;
    uint32_t      owner_pid;
    int           window_id; // Workspace ID in batch mode

    bool dirty;

    wm_window_t *prev;
    wm_window_t *next;
};

/* =========================================================================
 * Session API  (video-mode lifecycle)
 * =========================================================================
 *
 * wm_session_start() — NON-BLOCKING (async compositor).
 *   Enters VGA Mode 13h, registers an async compositor task that fires
 *   every 16 ms.  Returns immediately so the calling app / terminal
 *   can continue running.  All windows added via wm_create_window() are
 *   composited by this task.  wm_create_window() calls wm_session_start()
 *   automatically if no session is running yet.
 *   Exit: left-click the window or taskbar Close button.
 *
 * wm_session_run() — BLOCKING (single-app animated mode).
 *   Like wm_session_start() but blocks in a loop, calling draw_cb every
 *   frame so windows can animate.  Only the calling app's code is alive,
 *   so only its draw_cb pointers are guaranteed valid.
 *   Exit: same as above.
 *
 * wm_session_stop() — stop compositor (or blocking loop), destroy all
 *   windows, restore text mode.  Safe to call from event/draw callbacks.
 *
 * wm_session_active() — true while either mode is running.
 */
void wm_session_start(void);   /* async, non-blocking */
void wm_session_stop(void);
bool wm_session_active(void);

/* =========================================================================
 * Core window API
 * ========================================================================= */

/** wm_init - (Re)initialise window list without entering video mode. */
void wm_init(void);

wm_window_t *wm_create_window(const wm_window_options_t *opts);
void         wm_destroy_window(wm_window_t *win);
bool         wm_is_window_valid(wm_window_t *win);

int          wm_get_window_count(void);
int          wm_get_window_count_for_pid(uint32_t pid);
bool         wm_has_windows_for_pid(uint32_t pid);
wm_window_t *wm_get_window_list(void);
wm_window_t *wm_get_focused(void);
void         wm_set_focus(wm_window_t *win);
void         wm_focus_window_for_pid(uint32_t pid);
void         wm_focus_next(void);
void         wm_focus_prev(void);

void wm_invalidate(wm_window_t *win);
void wm_invalidate_all(void);
bool wm_poll_event(wm_window_t *win, wm_event_t *out);

void wm_set_fullscreen(wm_window_t *win, bool enable);
void wm_minimize(wm_window_t *win);
void wm_restore(wm_window_t *win);
void wm_toggle_maximize(wm_window_t *win);
void wm_move(wm_window_t *win, int16_t nx, int16_t ny);
bool wm_resize(wm_window_t *win, uint16_t nw, uint16_t nh);
void wm_set_title(wm_window_t *win, const char *title);
void wm_set_shape(wm_window_t *win, const uint8_t *mask, uint16_t mw, uint16_t mh);

/* =========================================================================
 * Drawing helpers  (operate on any uint8_t pixel buffer)
 * ========================================================================= */
void wm_buf_put_pixel   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, uint8_t color);
void wm_buf_draw_line   (uint8_t *buf, uint16_t bw, uint16_t bh, int x0, int y0, int x1, int y1, uint8_t color);
void wm_buf_fill_rect   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t color);
void wm_buf_draw_rect   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, int w, int h, uint8_t color);
void wm_buf_draw_char   (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char c,         uint8_t color);
void wm_buf_draw_char_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, char c,         uint8_t fg, uint8_t bg);
void wm_buf_draw_string (uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t color);
void wm_buf_draw_string_bg(uint8_t *buf, uint16_t bw, uint16_t bh, int x, int y, const char *str, uint8_t fg, uint8_t bg);

/* =========================================================================
 * Compositor tick  (called from terminal_console each kernel loop iteration)
 * =========================================================================
 *
 * Polls keyboard and mouse, composites all windows, draws cursor and the
 * system exit button, then flips the backbuffer to VRAM.
 * Stops the session when the window/taskbar Close button is clicked.
 *
 * Called automatically by terminal_console() when wm_session_active().
 * Kernel / driver code MUST NOT call this directly.
 */
void wm_compositor_tick(void);

/**
 * Get current display/compositor screen bounds in pixels.
 */
void wm_get_screen_bounds(int *out_w, int *out_h);

/**
 * Show or hide the WM system cursor while this window is focused and the
 * pointer is inside its client area. Applications that draw a custom cursor
 * or hide the cursor completely should pass visible = false.
 */
void wm_set_cursor_visible(wm_window_t *win, bool visible);

/**
 * Query whether this window leaves the WM system cursor visible when focused.
 */
bool wm_get_cursor_visible(const wm_window_t *win);

/**
 * Set or query the global WM cursor size in pixels (independent of screen resolution).
 * Both width and height must be positive integers (>= 1).
 */
bool wm_set_cursor_scale(int width, int height);
void wm_get_cursor_scale(int *out_width, int *out_height);

#endif /* LIB_WM_H */
