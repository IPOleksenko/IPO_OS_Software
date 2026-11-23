#include "cfg.h"

#ifdef GRDRV_IPO

#include "links.h"
#include "ipo_gui.h"
#include "ipo_js_engine.h"
#include "ipo_ui_font.h"
#include "ipo_crisp_fonts.h"
#include "video_player.h"
#include "youtube_client.h"
#include "mp4_player.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>

#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>
#include <ipo/gfx.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <driver/audio_core.h>
#include <system/state.h>
#include <syscall.h>

/* Avoid get_char() prototype conflict between links.h and keymap.h */
#define KBD_MOD_CTRL  0x01
#define KBD_MOD_SHIFT 0x02
#define KBD_MOD_ALT   0x04
extern uint8_t keyboard_get_modifiers(void);
extern const char *keyboard_get_key_string(uint8_t scancode);

/* =========================================================================
 * External / helper prototypes
 * ========================================================================= */
extern struct graphics_driver ipo_driver;

/* =========================================================================
 * Color Palette & Fast 32bpp -> 8bpp Quantizer
 * ========================================================================= */
#define COL_BG_DEEP      0x0F172Au
#define COL_TABBAR_BG    0x161B22u
#define COL_TAB_ACTIVE   0x212834u
#define COL_TAB_INACTIVE 0x1B222Cu
#define COL_TAB_HOVER    0x27303Eu
#define COL_TOOLBAR_BG   0x212834u
#define COL_BOOKMARK_BG  0x1A202Cu
#define COL_STATUSBAR_BG 0x11161Fu
#define COL_OMNIBOX_BG   0x0D1117u
#define COL_OMNIBOX_BD   0x30363Du
#define COL_CARD_BG      0x1E293Bu
#define COL_CARD_HOVER   0x273549u
#define COL_CARD_BD      0x334155u
#define COL_BTN_HOVER    0x2E3748u
#define COL_ACCENT_BLUE  0x2563EBu
#define COL_ACCENT_CYAN  0x38BDF8u
#define COL_ACCENT_GREEN 0x10B981u
#define COL_GREEN_DARK   0x064E3Bu
#define COL_ACCENT_RED   0xEF4444u
#define COL_RED_DARK     0x7F1D1Du
#define COL_ACCENT_GOLD  0xFBBF24u
#define COL_ACCENT_PURP  0xA855F7u
#define COL_TEXT_MAIN    0xF1F5F9u
#define COL_TEXT_SUB     0x94A3B8u
#define COL_TEXT_DIM     0x64748Bu
#define COL_BORDER       0x30363Du
#define COL_WHITE        0xFFFFFFu
#define COL_BLACK        0x000000u

static uint32_t *s_fb32 = NULL;
static uint8_t  *s_fb8_base = NULL;
static uint8_t  *s_lut_near = NULL;
static bool      s_use_truecolor32 = false;

static uint8_t s_pal_r[256];
static uint8_t s_pal_g[256];
static uint8_t s_pal_b[256];

static const int s_bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

/* =========================================================================
 * UI Geometry & State
 * ========================================================================= */
typedef enum {
    FOCUS_VIEWPORT = 0,
    FOCUS_OMNIBOX  = 1,
    FOCUS_HERO     = 2,
    FOCUS_FIND     = 3
} focus_target_t;

typedef enum {
    OVL_NONE      = 0,
    OVL_DOWNLOADS = 1,
    OVL_FIND      = 2,
    OVL_MENU      = 3,
    OVL_HISTORY   = 4,
    OVL_SECURITY  = 5,
    OVL_CONTEXT   = 6
} overlay_kind_t;

typedef enum {
    CURSOR_ARROW = 0,
    CURSOR_HAND  = 1,
    CURSOR_IBEAM = 2
} cursor_kind_t;

static const char *gui_str(const char *s) {
    return s ? s : "";
}

static bool gui_set_str(char **dst, const char *src) {
    if (!dst) return false;
    if (!src) src = "";
    size_t len = strlen(src);
    if (len == SIZE_MAX) return false;
    char *copy = (char *)malloc(len + 1u);
    if (!copy) return false;
    memcpy(copy, src, len + 1u);
    free(*dst);
    *dst = copy;
    return true;
}

static bool gui_set_strn(char **dst, const char *src, size_t len) {
    if (!dst) return false;
    if (!src) len = 0;
    if (len == SIZE_MAX) return false;
    char *copy = (char *)malloc(len + 1u);
    if (!copy) return false;
    if (len > 0) memcpy(copy, src, len);
    copy[len] = '\0';
    free(*dst);
    *dst = copy;
    return true;
}

static char *gui_fmt_alloc(const char *fmt, ...) {
    if (!fmt) return NULL;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return NULL;
    size_t sz = (size_t)n + 1u;
    char *buf = (char *)malloc(sz);
    if (!buf) return NULL;
    va_start(ap, fmt);
    vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    return buf;
}

static bool gui_set_fmt(char **dst, const char *fmt, ...) {
    if (!dst || !fmt) return false;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    size_t sz = (size_t)n + 1u;
    char *buf = (char *)malloc(sz);
    if (!buf) return false;
    va_start(ap, fmt);
    vsnprintf(buf, sz, fmt, ap);
    va_end(ap);
    free(*dst);
    *dst = buf;
    return true;
}

static bool gui_dyn_ensure(char **buf, size_t *cap, size_t need_bytes) {
    if (!buf || !cap || need_bytes == 0) return false;
    if (*buf && *cap >= need_bytes) return true;
    size_t new_cap = (*cap > 0) ? *cap : 64u;
    while (new_cap < need_bytes) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = need_bytes;
            break;
        }
        new_cap *= 2u;
    }
    char *nb = (char *)realloc(*buf, new_cap);
    if (!nb) return false;
    if (!*buf) nb[0] = '\0';
    *buf = nb;
    *cap = new_cap;
    return true;
}

static bool gui_dyn_set(char **buf, size_t *cap, int *out_len, const char *src) {
    if (!src) src = "";
    size_t slen = strlen(src);
    if (slen >= (size_t)0x7fffffff) return false;
    if (!gui_dyn_ensure(buf, cap, slen + 1u)) return false;
    memcpy(*buf, src, slen + 1u);
    if (out_len) *out_len = (int)slen;
    return true;
}

typedef struct {
    bool used;
    struct graphics_device *dev;
    bool is_home;
    bool is_video;
    char *title;
    char *url;
    char *video_url;
    char *video_title;
} ipo_tab_t;

typedef struct {
    char *label;
    char *url;
    uint32_t badge_color;
    char badge_char;
    bool custom;
} ipo_bookmark_t;

typedef struct {
    char *title;
    char *url;
    uint32_t visit_ms;
} ipo_hist_entry_t;

typedef struct {
    char *label;
    char *url;
    bool is_search;
    bool is_bookmark;
} ipo_ac_item_t;

/* Non-blocking GUI HTTP/HTTPS Download Slots */
typedef enum {
    DL_IDLE = 0,
    DL_CONNECTING,
    DL_TLS_HANDSHAKE,
    DL_READING,
    DL_DONE,
    DL_ERROR
} dl_state_t;

typedef struct {
    bool used;
    dl_state_t state;
    char *url;
    char *cur_url;
    char *filename;
    char *dest;
    char *host;
    char *path;
    int port;
    bool is_https;
    int redirects;
    char **redirect_history;
    size_t redirect_count;
    size_t redirect_cap;
    int sock;
    SSL_CTX *ctx;
    SSL *ssl;
    int fd;
    char *hdr_buf;
    size_t hdr_len;
    size_t hdr_cap;
    bool headers_done;
    int status_code;
    bool chunked;
    int chunk_state;
    size_t chunk_rem;
    char *chunk_line;
    size_t chunk_line_len;
    size_t chunk_line_cap;
    int64_t content_length;
    uint64_t downloaded_bytes;
    uint32_t start_ms;
    uint32_t end_ms;
    char *err_msg;
} dl_slot_t;

/* Embedded Video Player State */
static mp4_player_t *s_mp4_player = NULL;

static bool s_inited = false;
static bool s_shutting_down = false;
static bool s_direct_gfx = false;
static wm_window_t *s_wm_win = NULL;
static bool s_hw_direct_fb = false;
static int64_t s_prev_cm_x = 0;
static int64_t s_prev_cm_y = 0;
static int32_t s_prev_wm_mx = -1;
static int32_t s_prev_wm_my = -1;
static bool s_force_hw_reflip = false;
static int s_wm_last_w = 0;
static int s_wm_last_h = 0;
static int s_gui_w = IPO_GUI_DEFAULT_W;
static int s_gui_h = IPO_GUI_DEFAULT_H;

static bool s_fullscreen = false;
static bool s_dirty_chrome = true;
static bool s_dirty_vp = true;
static bool s_dirty_vp_rows = false;
static int  s_dirty_vp_y1 = 0;
static int  s_dirty_vp_y2 = 0;
static bool s_dirty_overlay = true;
static uint32_t s_last_present_ms = 0;
static int *s_wm_lut_x = NULL;
static int  s_wm_lut_x_w = 0;
static int  s_wm_lut_src_w = 0;

static inline bool gui_is_compact(void) {
    return (s_gui_w < 540 || s_gui_h < 280);
}

static inline bool gui_is_medium(void) {
    return (s_gui_w < 860 || s_gui_h < 480);
}

static inline int ui_is_compact_font(void) {
    return gui_is_compact();
}

static inline int ui_font_px(void) {
    if (gui_is_compact()) return 10;
    if (gui_is_medium()) return 12;
    return 14;
}

static inline int ui_char_w(void) {
    if (gui_is_compact()) return 6;
    if (gui_is_medium()) return 7;
    return 8;
}

static inline int ui_char_h(void) {
    return ui_font_px();
}

int ipo_gui_w(void) { return s_gui_w; }
int ipo_gui_h(void) { return s_gui_h; }
int ipo_tabbar_h(void) { return gui_is_compact() ? 16 : (gui_is_medium() ? 22 : 30); }
int ipo_toolbar_h(void) { return gui_is_compact() ? 18 : (gui_is_medium() ? 26 : 36); }
int ipo_bookmarks_h(void) { return (s_gui_h < 280) ? 0 : (gui_is_medium() ? 18 : 24); }
int ipo_top_chrome(void) { return ipo_tabbar_h() + ipo_toolbar_h() + ipo_bookmarks_h(); }
int ipo_statusbar_h(void) { return gui_is_compact() ? 14 : (gui_is_medium() ? 16 : 22); }

static ipo_tab_t *s_tabs = NULL;
static int s_tabs_cap = 0;
static int s_active_tab = 0;
static int s_tab_scroll = 0;

static ipo_bookmark_t *s_bookmarks = NULL;
static int s_bookmark_count = 0;
static int s_bookmark_cap = 0;

static bool ensure_bookmark_cap(int need) {
    if (need <= s_bookmark_cap) return true;
    int new_cap = s_bookmark_cap ? s_bookmark_cap : 16;
    while (new_cap < need) {
        if (new_cap > INT32_MAX / 2) return false;
        new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(ipo_bookmark_t)) return false;
    ipo_bookmark_t *nb = (ipo_bookmark_t *)realloc(s_bookmarks, (size_t)new_cap * sizeof(ipo_bookmark_t));
    if (!nb) return false;
    memset(nb + s_bookmark_cap, 0, (size_t)(new_cap - s_bookmark_cap) * sizeof(ipo_bookmark_t));
    s_bookmarks = nb;
    s_bookmark_cap = new_cap;
    return true;
}

static ipo_hist_entry_t *s_history = NULL;
static int s_history_count = 0;
static int s_history_cap = 0;

static ipo_ac_item_t *s_ac_items = NULL;
static int s_ac_count = 0;
static int s_ac_cap = 0;
static int s_ac_sel = -1;

static int s_zoom_pct = 100;
static int s_ctx_x = 200, s_ctx_y = 200;
static bool s_ctx_has_link = false;
static char *s_ctx_link_url = NULL;
static bool s_dragging_vscroll = false;
static int  s_dragging_textbox = 0; /* 1=omnibox, 2=hero, 3=find */

static dl_slot_t *s_dls = NULL;
static int s_dls_cap = 0;

static focus_target_t s_focus = FOCUS_VIEWPORT;
static overlay_kind_t s_overlay = OVL_NONE;
static cursor_kind_t s_cursor = CURSOR_ARROW;

static char  *s_omnibox_text = NULL;
static size_t s_omnibox_cap = 0;
static int    s_omnibox_len = 0;
static int    s_omnibox_cursor = 0;
static int    s_omnibox_scroll = 0;
static bool   s_omnibox_sel_all = false;
static uint32_t s_omnibox_last_click_ms = 0;

static char  *s_hero_text = NULL;
static size_t s_hero_cap = 0;
static int    s_hero_len = 0;
static int    s_hero_cursor = 0;
static int    s_hero_scroll = 0;
static bool   s_hero_sel_all = false;
static uint32_t s_hero_last_click_ms = 0;

static char  *s_find_text = NULL;
static size_t s_find_cap = 0;
static int    s_find_len = 0;
static int    s_find_cursor = 0;
static int    s_find_scroll = 0;
static bool   s_find_sel_all = false;
static uint32_t s_find_last_click_ms = 0;

static char *s_status_msg = NULL;
static char *s_pending_initial_video = NULL;
static char *s_pending_tab_url = NULL;

static void tab_free_fields(ipo_tab_t *tab) {
    if (!tab) return;
    free(tab->title);
    free(tab->url);
    free(tab->video_url);
    free(tab->video_title);
    tab->title = NULL;
    tab->url = NULL;
    tab->video_url = NULL;
    tab->video_title = NULL;
}

static void ac_clear_items(void) {
    for (int i = 0; i < s_ac_count; i++) {
        free(s_ac_items[i].label);
        free(s_ac_items[i].url);
        s_ac_items[i].label = NULL;
        s_ac_items[i].url = NULL;
    }
    s_ac_count = 0;
}

static bool ac_ensure_cap(int need) {
    if (need <= s_ac_cap) return true;
    int new_cap = (s_ac_cap > 0) ? s_ac_cap : 8;
    while (new_cap < need) {
        if (new_cap > 0x3fffffff) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }
    ipo_ac_item_t *arr = (ipo_ac_item_t *)realloc(s_ac_items, (size_t)new_cap * sizeof(ipo_ac_item_t));
    if (!arr) return false;
    memset(arr + s_ac_cap, 0, (size_t)(new_cap - s_ac_cap) * sizeof(ipo_ac_item_t));
    s_ac_items = arr;
    s_ac_cap = new_cap;
    return true;
}

static void dl_slot_free_redirect_history(dl_slot_t *dl) {
    if (!dl) return;
    for (size_t i = 0; i < dl->redirect_count; i++) {
        free(dl->redirect_history[i]);
    }
    free(dl->redirect_history);
    dl->redirect_history = NULL;
    dl->redirect_count = 0;
    dl->redirect_cap = 0;
}

static bool dl_slot_seen_url(const dl_slot_t *dl, const char *url) {
    if (!dl || !url) return false;
    for (size_t i = 0; i < dl->redirect_count; i++) {
        if (dl->redirect_history[i] && strcmp(dl->redirect_history[i], url) == 0) return true;
    }
    return false;
}

static bool dl_slot_record_url(dl_slot_t *dl, const char *url) {
    if (!dl || !url) return false;
    if (dl->redirect_count >= dl->redirect_cap) {
        size_t new_cap = dl->redirect_cap ? dl->redirect_cap * 2u : 8u;
        if (new_cap > SIZE_MAX / sizeof(char *)) return false;
        char **arr = (char **)realloc(dl->redirect_history, new_cap * sizeof(char *));
        if (!arr) return false;
        dl->redirect_history = arr;
        dl->redirect_cap = new_cap;
    }
    size_t len = strlen(url);
    if (len == SIZE_MAX) return false;
    char *copy = (char *)malloc(len + 1u);
    if (!copy) return false;
    memcpy(copy, url, len + 1u);
    dl->redirect_history[dl->redirect_count++] = copy;
    return true;
}

static void dl_slot_free_strings(dl_slot_t *dl) {
    if (!dl) return;
    free(dl->url);
    free(dl->cur_url);
    free(dl->filename);
    free(dl->dest);
    free(dl->host);
    free(dl->path);
    free(dl->hdr_buf);
    free(dl->chunk_line);
    free(dl->err_msg);
    dl_slot_free_redirect_history(dl);
    dl->url = NULL;
    dl->cur_url = NULL;
    dl->filename = NULL;
    dl->dest = NULL;
    dl->host = NULL;
    dl->path = NULL;
    dl->hdr_buf = NULL;
    dl->hdr_cap = 0;
    dl->hdr_len = 0;
    dl->chunk_line = NULL;
    dl->chunk_line_cap = 0;
    dl->chunk_line_len = 0;
    dl->err_msg = NULL;
}

/* Mouse & Keyboard state tracking */
static int  s_mx = 400, s_my = 300;
static bool s_btn_l = false, s_btn_r = false, s_btn_m = false;
static int32_t s_prev_scroll_pos = 0;
static int  s_hover_id = 0;
static bool s_scancode_e0 = false;
static bool s_mod_ctrl = false, s_mod_shift = false, s_mod_alt = false, s_mod_caps = false;

/* Video player state */
static bool s_vid_paused = false;
static bool s_vid_muted = false;
static uint32_t s_vid_pos_ms = 0;
static uint32_t s_vid_last_tick_ms = 0;
static int  s_vid_dragging_seek = 0;
static bool s_links_wakeup = false;

/* =========================================================================
 * Timing & String Helpers
 * ========================================================================= */
static inline uint32_t gui_millis(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
}

static const char *ci_strstr_local(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return hay;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nl) return p;
    }
    return NULL;
}

static int ensure_tab_slot(void) {
    for (int i = 0; i < s_tabs_cap; i++) {
        if (!s_tabs[i].used) return i;
    }
    int new_cap = (s_tabs_cap > 0) ? (s_tabs_cap * 2) : 8;
    ipo_tab_t *nt = (ipo_tab_t *)realloc(s_tabs, (size_t)new_cap * sizeof(ipo_tab_t));
    if (!nt) return -1;
    memset(nt + s_tabs_cap, 0, (size_t)(new_cap - s_tabs_cap) * sizeof(ipo_tab_t));
    int idx = s_tabs_cap;
    s_tabs = nt;
    s_tabs_cap = new_cap;
    return idx;
}

static int tab_count(void) {
    int n = 0;
    for (int i = 0; i < s_tabs_cap; i++) {
        if (s_tabs[i].used) n++;
    }
    return n;
}

static struct terminal *dev_to_term(struct graphics_device *dev) {
    if (!dev) return NULL;
    return (struct terminal *)dev->user_data;
}

static struct session *dev_to_session(struct graphics_device *dev) {
    struct terminal *term = dev_to_term(dev);
    if (!term) return NULL;
    struct window *win;
    struct list_head *lw;
    foreachback(struct window, win, lw, term->windows) {
        if (win->handler == win_func && win->data) {
            return (struct session *)win->data;
        }
    }
    return NULL;
}

/* =========================================================================
 * Cyrillic + UTF-8 8x16 Font Table
 * ========================================================================= */
static const uint8_t s_cyr_upper[32][16] = {
    /* U+0410 */ {0x00,0x00,0x18,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+0411 */ {0x00,0x00,0x7E,0x60,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* U+0412 */ {0x00,0x00,0x7C,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* U+0413 */ {0x00,0x00,0x7E,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00},
    /* U+0414 */ {0x00,0x00,0x1E,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x7F,0x63,0x63,0x00,0x00,0x00},
    /* U+0415 */ {0x00,0x00,0x7E,0x60,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x7E,0x00,0x00,0x00,0x00},
    /* U+0416 */ {0x00,0x00,0x63,0x6B,0x6B,0x3E,0x1C,0x3E,0x6B,0x6B,0x6B,0x63,0x00,0x00,0x00,0x00},
    /* U+0417 */ {0x00,0x00,0x3C,0x66,0x06,0x06,0x1C,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+0418 */ {0x00,0x00,0x66,0x66,0x66,0x6E,0x7E,0x76,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+0419 */ {0x00,0x18,0x00,0x66,0x66,0x6E,0x7E,0x76,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+041A */ {0x00,0x00,0x66,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+041B */ {0x00,0x00,0x1E,0x36,0x36,0x36,0x36,0x36,0x36,0x36,0x66,0xC6,0x00,0x00,0x00,0x00},
    /* U+041C */ {0x00,0x00,0xC3,0xE7,0xFF,0xFF,0xDB,0xC3,0xC3,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00},
    /* U+041D */ {0x00,0x00,0x66,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+041E */ {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+041F */ {0x00,0x00,0x7E,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+0420 */ {0x00,0x00,0x7C,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00},
    /* U+0421 */ {0x00,0x00,0x3C,0x66,0x60,0x60,0x60,0x60,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+0422 */ {0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    /* U+0423 */ {0x00,0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x18,0x30,0x60,0x60,0x00,0x00,0x00,0x00},
    /* U+0424 */ {0x00,0x00,0x18,0x7E,0xDB,0xDB,0xDB,0xDB,0x7E,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    /* U+0425 */ {0x00,0x00,0x66,0x66,0x3C,0x18,0x18,0x3C,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+0426 */ {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x7F,0x03,0x03,0x00,0x00,0x00},
    /* U+0427 */ {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x06,0x06,0x06,0x00,0x00,0x00,0x00},
    /* U+0428 */ {0x00,0x00,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xFF,0x00,0x00,0x00,0x00},
    /* U+0429 */ {0x00,0x00,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xFF,0x03,0x03,0x00,0x00,0x00},
    /* U+042A */ {0x00,0x00,0xF0,0x30,0x30,0x30,0x3C,0x36,0x36,0x36,0x36,0x3C,0x00,0x00,0x00,0x00},
    /* U+042B */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xF6,0xCE,0xCE,0xCE,0xCE,0xF6,0x00,0x00,0x00,0x00},
    /* U+042C */ {0x00,0x00,0x60,0x60,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* U+042D */ {0x00,0x00,0x3C,0x66,0x06,0x06,0x3E,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+042E */ {0x00,0x00,0xCE,0xDB,0xDB,0xDB,0xFB,0xDB,0xDB,0xDB,0xDB,0xCE,0x00,0x00,0x00,0x00},
    /* U+042F */ {0x00,0x00,0x3E,0x66,0x66,0x66,0x3E,0x1E,0x36,0x66,0x66,0x66,0x00,0x00,0x00,0x00}
};

static const uint8_t s_cyr_lower[32][16] = {
    /* U+0430 */ {0x00,0x00,0x00,0x00,0x00,0x3C,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00,0x00,0x00,0x00},
    /* U+0431 */ {0x00,0x00,0x0E,0x1C,0x60,0x7C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+0432 */ {0x00,0x00,0x00,0x00,0x00,0x7C,0x66,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* U+0433 */ {0x00,0x00,0x00,0x00,0x00,0x7E,0x60,0x60,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00},
    /* U+0434 */ {0x00,0x00,0x00,0x00,0x00,0x3E,0x36,0x36,0x36,0x36,0x7F,0x63,0x63,0x00,0x00,0x00},
    /* U+0435 */ {0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x7E,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+0436 */ {0x00,0x00,0x00,0x00,0x00,0xDB,0x5A,0x3C,0x3C,0x5A,0xDB,0xDB,0x00,0x00,0x00,0x00},
    /* U+0437 */ {0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x1C,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+0438 */ {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x6E,0x7E,0x76,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+0439 */ {0x00,0x00,0x18,0x00,0x00,0x66,0x66,0x6E,0x7E,0x76,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+043A */ {0x00,0x00,0x00,0x00,0x00,0x66,0x6C,0x78,0x78,0x6C,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+043B */ {0x00,0x00,0x00,0x00,0x00,0x3E,0x36,0x36,0x36,0x36,0x66,0xC6,0x00,0x00,0x00,0x00},
    /* U+043C */ {0x00,0x00,0x00,0x00,0x00,0xC3,0xE7,0xFF,0xDB,0xC3,0xC3,0xC3,0x00,0x00,0x00,0x00},
    /* U+043D */ {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+043E */ {0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+043F */ {0x00,0x00,0x00,0x00,0x00,0x7E,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+0440 */ {0x00,0x00,0x00,0x00,0x00,0x7C,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x00,0x00,0x00},
    /* U+0441 */ {0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+0442 */ {0x00,0x00,0x00,0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    /* U+0443 */ {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00,0x00,0x00},
    /* U+0444 */ {0x00,0x00,0x00,0x18,0x00,0x3C,0x5A,0xDB,0xDB,0x5A,0x3C,0x18,0x18,0x00,0x00,0x00},
    /* U+0445 */ {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0x18,0x18,0x3C,0x66,0x66,0x00,0x00,0x00,0x00},
    /* U+0446 */ {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x7F,0x03,0x03,0x00,0x00,0x00},
    /* U+0447 */ {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x06,0x06,0x00,0x00,0x00,0x00},
    /* U+0448 */ {0x00,0x00,0x00,0x00,0x00,0xDB,0xDB,0xDB,0xDB,0xDB,0xDB,0xFF,0x00,0x00,0x00,0x00},
    /* U+0449 */ {0x00,0x00,0x00,0x00,0x00,0xDB,0xDB,0xDB,0xDB,0xDB,0xFF,0x03,0x03,0x00,0x00,0x00},
    /* U+044A */ {0x00,0x00,0x00,0x00,0x00,0xF0,0x30,0x3C,0x36,0x36,0x36,0x3C,0x00,0x00,0x00,0x00},
    /* U+044B */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xF6,0xCE,0xCE,0xCE,0xF6,0x00,0x00,0x00,0x00},
    /* U+044C */ {0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* U+044D */ {0x00,0x00,0x00,0x00,0x00,0x3C,0x66,0x06,0x3E,0x06,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* U+044E */ {0x00,0x00,0x00,0x00,0x00,0xCE,0xDB,0xFB,0xDB,0xDB,0xDB,0xCE,0x00,0x00,0x00,0x00},
    /* U+044F */ {0x00,0x00,0x00,0x00,0x00,0x3E,0x66,0x3E,0x1E,0x36,0x66,0x66,0x00,0x00,0x00,0x00}
};

static const uint8_t s_cyr_yo_upper[16] = {0x00,0x24,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x7E,0x00,0x00,0x00,0x00};
static const uint8_t s_cyr_yo_lower[16] = {0x00,0x00,0x24,0x00,0x00,0x3C,0x66,0x7E,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00};
static const uint8_t s_glyph_bullet[16] = {0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x3C,0x3C,0x18,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t s_glyph_ellipsis[16]={0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xDB,0xDB,0x00,0x00,0x00,0x00};

static const uint8_t *get_glyph_32(uint32_t cp) {
    if (cp < 0x80u) return vga_default_font_get_glyph((uint8_t)cp);
    if (cp == 0x0401u) return s_cyr_yo_upper;
    if (cp == 0x0451u) return s_cyr_yo_lower;
    if (cp >= 0x0410u && cp <= 0x042Fu) return s_cyr_upper[cp - 0x0410u];
    if (cp >= 0x0430u && cp <= 0x044Fu) return s_cyr_lower[cp - 0x0430u];
    if (cp == 0x2022u || cp == 0x00B7u) return s_glyph_bullet;
    if (cp == 0x2026u) return s_glyph_ellipsis;
    if (cp == 0x2014u || cp == 0x2013u) return vga_default_font_get_glyph('-');
    return vga_default_font_get_glyph('?');
}

static uint32_t utf8_next(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    if (!s || !*s) return 0;
    unsigned char b0 = s[0];
    if (b0 < 0x80u) {
        *p = (const char *)(s + 1);
        return b0;
    }
    if ((b0 & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
        *p = (const char *)(s + 2);
        return ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(s[1] & 0x3Fu);
    }
    if ((b0 & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
        *p = (const char *)(s + 3);
        return ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (uint32_t)(s[2] & 0x3Fu);
    }
    if ((b0 & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u && (s[3] & 0xC0u) == 0x80u) {
        *p = (const char *)(s + 4);
        return ((uint32_t)(b0 & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) |
               ((uint32_t)(s[2] & 0x3Fu) << 6) | (uint32_t)(s[3] & 0x3Fu);
    }
    *p = (const char *)(s + 1);
    return '?';
}

static int utf8_total_cols(const char *s, int byte_len) {
    if (!s || byte_len <= 0) return 0;
    int cols = 0;
    const char *p = s;
    const char *end = s + byte_len;
    while (p < end && *p) {
        utf8_next(&p);
        cols++;
    }
    return cols;
}

static int utf8_byte_to_col(const char *s, int byte_off) {
    if (!s || byte_off <= 0) return 0;
    int cols = 0;
    const char *p = s;
    const char *end = s + byte_off;
    while (p < end && *p) {
        utf8_next(&p);
        cols++;
    }
    return cols;
}

static int utf8_col_to_byte(const char *s, int byte_len, int target_col) {
    if (!s || byte_len <= 0 || target_col <= 0) return 0;
    const char *p = s;
    const char *end = s + byte_len;
    int col = 0;
    while (p < end && *p && col < target_col) {
        utf8_next(&p);
        col++;
    }
    int off = (int)(p - s);
    if (off > byte_len) off = byte_len;
    return off;
}

static inline int ui_glyph_adv(uint32_t cp) {
    if (ipo_font_is_zero_width(cp) || ipo_font_is_combining(cp)) return 0;
    int a = ipo_font_measure_glyph(IPO_FONT_REGULAR, ui_font_px(), cp);
    if (a < 0) a = ipo_font_measure_fallback(ui_font_px());
    if (a < 1) a = ui_char_w();
    return a;
}

static int ui_text_w(const char *utf8) {
    if (!utf8 || !*utf8) return 0;
    return ipo_font_text_width(IPO_FONT_REGULAR, ui_font_px(), utf8);
}

static int ui_substr_w(const char *buf, int len, int start_col, int end_col) {
    if (!buf || len <= 0) return 0;
    if (start_col < 0) start_col = 0;
    if (end_col <= start_col) return 0;
    int start_byte = utf8_col_to_byte(buf, len, start_col);
    const char *p = buf + start_byte;
    const char *end = buf + len;
    int col = start_col, w = 0;
    while (p < end && col < end_col) {
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        w += ui_glyph_adv(cp);
        col++;
    }
    return w;
}

static void textbox_sync_scroll(const char *buf, int len, int cursor, int *scroll_col, int max_w) {
    if (!scroll_col) return;
    int avail_w = max_w - 6;
    if (avail_w < 8) avail_w = 8;
    int total_cols = utf8_total_cols(buf, len);
    int cur_col = utf8_byte_to_col(buf, cursor);
    if (cur_col < 0) cur_col = 0;
    if (cur_col > total_cols) cur_col = total_cols;
    if (*scroll_col > total_cols) *scroll_col = total_cols;
    if (*scroll_col < 0) *scroll_col = 0;
    if (cur_col < *scroll_col) *scroll_col = cur_col;
    while (*scroll_col < cur_col && ui_substr_w(buf, len, *scroll_col, cur_col) > avail_w) {
        (*scroll_col)++;
    }
    while (*scroll_col > 0 && ui_substr_w(buf, len, *scroll_col - 1, total_cols) <= avail_w) {
        (*scroll_col)--;
    }
}

static int textbox_cursor_from_x(const char *buf, int len, int scroll_col, int text_x, int mx) {
    if (mx <= text_x) return utf8_col_to_byte(buf, len, scroll_col);
    int sb = utf8_col_to_byte(buf, len, scroll_col);
    const char *p = buf + sb;
    const char *end = buf + len;
    int px = text_x;
    while (p < end) {
        const char *char_start = p;
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        int adv = ui_glyph_adv(cp);
        if (mx < px + adv / 2) {
            return (int)(char_start - buf);
        }
        px += adv;
    }
    return len;
}

static bool is_url_word_sep(unsigned char c) {
    return (c <= ' ' || c == '/' || c == '.' || c == '?' || c == '&' ||
            c == '=' || c == ':' || c == '#' || c == '-' || c == '_' || c == '+');
}

static void utf8_jump_word_left(const char *buf, int *cursor) {
    int p = *cursor;
    while (p > 0) {
        int prev = p - 1;
        while (prev > 0 && ((unsigned char)buf[prev] & 0xC0u) == 0x80u) prev--;
        if (!is_url_word_sep((unsigned char)buf[prev])) break;
        p = prev;
    }
    while (p > 0) {
        int prev = p - 1;
        while (prev > 0 && ((unsigned char)buf[prev] & 0xC0u) == 0x80u) prev--;
        if (is_url_word_sep((unsigned char)buf[prev])) break;
        p = prev;
    }
    *cursor = p;
}

static void utf8_jump_word_right(const char *buf, int len, int *cursor) {
    int p = *cursor;
    while (p < len && !is_url_word_sep((unsigned char)buf[p])) {
        p++;
        while (p < len && ((unsigned char)buf[p] & 0xC0u) == 0x80u) p++;
    }
    while (p < len && is_url_word_sep((unsigned char)buf[p])) {
        p++;
        while (p < len && ((unsigned char)buf[p] & 0xC0u) == 0x80u) p++;
    }
    *cursor = p;
}

/* =========================================================================
 * Palette & Quantizer
 * ========================================================================= */
static void set_pal_entry(int idx, uint8_t r, uint8_t g, uint8_t b) {
    s_pal_r[idx] = r;
    s_pal_g[idx] = g;
    s_pal_b[idx] = b;
}

static uint8_t s_u8_to_u5[256];

static int nearest_cube6_idx(int v) {
    static const uint8_t cube6[6] = { 0, 48, 103, 151, 200, 255 };
    int best = 0;
    int best_d = abs(v - (int)cube6[0]);
    for (int i = 1; i < 6; i++) {
        int d = abs(v - (int)cube6[i]);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static void init_palette_and_lut(void) {
    static const uint8_t cga[16][3] = {
        {0,0,0},     {0,0,168},   {0,168,0},   {0,168,168},
        {168,0,0},   {168,0,168}, {168,87,0},  {168,168,168},
        {87,87,87},  {87,87,255}, {87,255,87}, {87,255,255},
        {255,87,87}, {255,87,255},{255,255,87},{255,255,255}
    };
    static const uint8_t gray16[16] = {
        0, 16, 32, 48, 64, 87, 103, 119,
        135, 151, 168, 184, 200, 216, 232, 255
    };
    static const uint8_t cube6[6] = { 0, 48, 103, 151, 200, 255 };

    for (int i = 0; i < 256; i++) {
        s_u8_to_u5[i] = (uint8_t)((i * 31 + 127) / 255);
    }

    for (int i = 0; i < 16; i++) {
        set_pal_entry(i, cga[i][0], cga[i][1], cga[i][2]);
    }
    for (int i = 0; i < 16; i++) {
        uint8_t v = gray16[i];
        set_pal_entry(16 + i, v, v, v);
    }
    int idx = 32;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                set_pal_entry(idx++, cube6[r], cube6[g], cube6[b]);
            }
        }
    }

    /* Dedicated crisp UI chrome DAC entries (no Bayer dither on modern dark slate chrome) */
    set_pal_entry(248, 0x0F, 0x17, 0x2A); /* COL_BG_DEEP */
    set_pal_entry(249, 0x16, 0x1B, 0x22); /* COL_TABBAR_BG */
    set_pal_entry(250, 0x21, 0x28, 0x34); /* COL_TAB_ACTIVE / COL_TOOLBAR_BG */
    set_pal_entry(251, 0x1E, 0x29, 0x3B); /* COL_CARD_BG / COL_TAB_INACTIVE */
    set_pal_entry(252, 0x0D, 0x11, 0x17); /* COL_OMNIBOX_BG */
    set_pal_entry(253, 0x25, 0x63, 0xEB); /* COL_ACCENT_BLUE */
    set_pal_entry(254, 0x38, 0xBD, 0xF8); /* COL_ACCENT_CYAN */
    set_pal_entry(255, 0xF1, 0xF5, 0xF9); /* COL_TEXT_MAIN */

    for (int r5 = 0; r5 < 32; r5++) {
        int r = (r5 * 255 + 15) / 31;
        int rc = nearest_cube6_idx(r);
        for (int g5 = 0; g5 < 32; g5++) {
            int g = (g5 * 255 + 15) / 31;
            int gc = nearest_cube6_idx(g);
            for (int b5 = 0; b5 < 32; b5++) {
                int b = (b5 * 255 + 15) / 31;
                int bc = nearest_cube6_idx(b);
                int cube_idx = 32 + rc * 36 + gc * 6 + bc;
                int dr = r - s_pal_r[cube_idx];
                int dg = g - s_pal_g[cube_idx];
                int db = b - s_pal_b[cube_idx];
                int best = cube_idx;
                int best_d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                if (best_d != 0) {
                    int max_extra = s_direct_gfx ? 256 : 32;
                    for (int p = 0; p < max_extra; p++) {
                        if (p == 32) {
                            p = 247;
                            continue;
                        }
                        dr = r - s_pal_r[p];
                        dg = g - s_pal_g[p];
                        db = b - s_pal_b[p];
                        int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                        if (d < best_d || (d == best_d && p < best)) {
                            best_d = d;
                            best = p;
                        }
                    }
                }
                s_lut_near[(r5 << 10) | (g5 << 5) | b5] = (uint8_t)best;
            }
        }
    }
}

static void apply_direct_ui_palette(void) {
    if (!s_direct_gfx) return;
    for (int i = 248; i < 256; i++) {
        vga_gfx_set_palette((uint8_t)i,
                            (uint8_t)(s_pal_r[i] >> 2),
                            (uint8_t)(s_pal_g[i] >> 2),
                            (uint8_t)(s_pal_b[i] >> 2));
    }
}

static inline uint8_t lut_lookup_rgb(int r, int g, int b) {
    return s_lut_near[((uint32_t)s_u8_to_u5[r] << 10) |
                      ((uint32_t)s_u8_to_u5[g] << 5) |
                      (uint32_t)s_u8_to_u5[b]];
}

static void quantize_rect(int x1, int y1, int x2, int y2, bool allow_dither) {
    if (s_use_truecolor32) return;
    if (!s_fb32 || !s_fb8_base) return;
    int gw = s_gui_w;
    int gh = s_gui_h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > gw) x2 = gw;
    if (y2 > gh) y2 = gh;
    if (x1 >= x2 || y1 >= y2) return;

    for (int y = y1; y < y2; y++) {
        const uint32_t *src = s_fb32 + y * gw;
        uint8_t *dst = s_fb8_base + y * gw;
        const int *brow = s_bayer4[y & 3];
        uint32_t prev_raw = 0xFFFFFFFFu;
        uint8_t prev_pal = 0;
        uint8_t dith4[4] = {0, 0, 0, 0};
        uint8_t mode = 0; /* 0 = none, 1 = solid, 2 = single-dither, 3 = dith4 ready */
        for (int x = x1; x < x2; x++) {
            uint32_t raw = src[x];
            if (raw == prev_raw) {
                if (mode == 1) {
                    dst[x] = prev_pal;
                    continue;
                }
                if (mode == 3) {
                    dst[x] = dith4[x & 3];
                    continue;
                }
                if (mode == 2) {
                    int r0 = (int)((raw >> 16) & 0xFFu);
                    int g0 = (int)((raw >> 8) & 0xFFu);
                    int b0 = (int)(raw & 0xFFu);
                    for (int k = 0; k < 4; k++) {
                        int bias = ((brow[k] - 7) * 15) >> 3;
                        int rk = r0 + bias; if (rk < 0) rk = 0; else if (rk > 255) rk = 255;
                        int gk = g0 + bias; if (gk < 0) gk = 0; else if (gk > 255) gk = 255;
                        int bk = b0 + bias; if (bk < 0) bk = 0; else if (bk > 255) bk = 255;
                        dith4[k] = lut_lookup_rgb(rk, gk, bk);
                    }
                    dst[x] = dith4[x & 3];
                    mode = 3;
                    continue;
                }
            }
            int r = (int)((raw >> 16) & 0xFFu);
            int g = (int)((raw >> 8) & 0xFFu);
            int b = (int)(raw & 0xFFu);
            uint8_t base = lut_lookup_rgb(r, g, b);
            bool px_dither = allow_dither && ((raw & IPO_PX_DITHER) != 0u);
            if (!px_dither) {
                dst[x] = base;
                prev_raw = raw;
                prev_pal = base;
                mode = 1;
                continue;
            }
            int dr = r - s_pal_r[base];
            int dg = g - s_pal_g[base];
            int db = b - s_pal_b[base];
            if ((dr * dr + dg * dg + db * db) <= 500) {
                dst[x] = base;
                prev_raw = raw;
                prev_pal = base;
                mode = 1;
                continue;
            }
            int bias = ((brow[x & 3] - 7) * 15) >> 3;
            r += bias; if (r < 0) r = 0; else if (r > 255) r = 255;
            g += bias; if (g < 0) g = 0; else if (g > 255) g = 255;
            b += bias; if (b < 0) b = 0; else if (b > 255) b = 255;
            dst[x] = lut_lookup_rgb(r, g, b);
            prev_raw = raw;
            mode = 2;
        }
    }
}

static void quantize_rows(int y1, int y2, bool allow_dither) {
    quantize_rect(0, y1, s_gui_w, y2, allow_dither);
}

/* =========================================================================
 * 32bpp Vector & Text Drawing Primitives
 * ========================================================================= */
static inline void put_px32(int x, int y, uint32_t col) {
    if ((unsigned)x < (unsigned)s_gui_w && (unsigned)y < (unsigned)s_gui_h) {
        s_fb32[y * s_gui_w + x] = col & 0x00FFFFFFu;
    }
}

static uint32_t lerp_color(uint32_t c1, uint32_t c2, int t256) {
    if (t256 <= 0) return c1;
    if (t256 >= 256) return c2;
    int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    int r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    int r = r1 + (((r2 - r1) * t256) >> 8);
    int g = g1 + (((g2 - g1) * t256) >> 8);
    int b = b1 + (((b2 - b1) * t256) >> 8);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void fill_rect32(int x, int y, int w, int h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = (x + w > IPO_GUI_W) ? IPO_GUI_W : (x + w);
    int y2 = (y + h > IPO_GUI_H) ? IPO_GUI_H : (y + h);
    uint32_t c = col & 0x00FFFFFFu;
    for (int ry = y1; ry < y2; ry++) {
        uint32_t *row = s_fb32 + ry * IPO_GUI_W;
        for (int rx = x1; rx < x2; rx++) {
            row[rx] = c;
        }
    }
}

static void stroke_rect32(int x, int y, int w, int h, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    fill_rect32(x, y, w, 1, col);
    fill_rect32(x, y + h - 1, w, 1, col);
    fill_rect32(x, y, 1, h, col);
    fill_rect32(x + w - 1, y, 1, h, col);
}

static void fill_rrect32(int x, int y, int w, int h, int r, uint32_t col) {
    if (w <= 0 || h <= 0) return;
    if (r <= 1 || r * 2 >= w || r * 2 >= h) {
        fill_rect32(x, y, w, h, col);
        return;
    }
    int r2 = r * r;
    for (int ry = 0; ry < h; ry++) {
        int inset = 0;
        if (ry < r) {
            int dy = r - ry - 1;
            while (inset < r && (r - inset) * (r - inset) + dy * dy > r2) inset++;
        } else if (ry >= h - r) {
            int dy = ry - (h - r);
            while (inset < r && (r - inset) * (r - inset) + dy * dy > r2) inset++;
        }
        fill_rect32(x + inset, y + ry, w - inset * 2, 1, col);
    }
}

static void stroke_rrect32(int x, int y, int w, int h, int r, uint32_t col) {
    if (w < 4 || h < 4) {
        stroke_rect32(x, y, w, h, col);
        return;
    }
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    fill_rect32(x + r, y, w - 2 * r, 1, col);
    fill_rect32(x + r, y + h - 1, w - 2 * r, 1, col);
    fill_rect32(x, y + r, 1, h - 2 * r, col);
    fill_rect32(x + w - 1, y + r, 1, h - 2 * r, col);
    int cx1 = x + r, cy1 = y + r;
    int cx2 = x + w - r - 1, cy2 = y + h - r - 1;
    for (int dy = 0; dy <= r; dy++) {
        int dx = (int)sqrt((double)(r * r - dy * dy) + 0.5);
        put_px32(cx1 - dx, cy1 - dy, col);
        put_px32(cx2 + dx, cy1 - dy, col);
        put_px32(cx1 - dx, cy2 + dy, col);
        put_px32(cx2 + dx, cy2 + dy, col);
        put_px32(cx1 - dy, cy1 - dx, col);
        put_px32(cx2 + dy, cy1 - dx, col);
        put_px32(cx1 - dy, cy2 + dx, col);
        put_px32(cx2 + dy, cy2 + dx, col);
    }
}

static inline void blend_px32(int x, int y, uint32_t col, uint8_t alpha) {
    if ((unsigned)x >= (unsigned)s_gui_w || (unsigned)y >= (unsigned)s_gui_h || alpha == 0) return;
    if (alpha == 255) {
        s_fb32[y * s_gui_w + x] = col & 0x00FFFFFFu;
        return;
    }
    uint32_t bg = s_fb32[y * s_gui_w + x];
    int br = (int)((bg >> 16) & 0xFFu), bgc = (int)((bg >> 8) & 0xFFu), bb = (int)(bg & 0xFFu);
    int fr = (int)((col >> 16) & 0xFFu), fgc = (int)((col >> 8) & 0xFFu), fb = (int)(col & 0xFFu);
    int a = (int)alpha, ia = 255 - a;
    int r = (fr * a + br * ia + 127) / 255;
    int g = (fgc * a + bgc * ia + 127) / 255;
    int b = (fb * a + bb * ia + 127) / 255;
    s_fb32[y * s_gui_w + x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void fill_circle32(int cx, int cy, int r, uint32_t col) {
    if (r <= 0) { put_px32(cx, cy, col); return; }
    if (r <= 2) {
        int r2 = r * r;
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (dx * dx + dy * dy <= r2) {
                    put_px32(cx + dx, cy + dy, col);
                }
            }
        }
        return;
    }
    double r_in = (double)r - 0.45;
    double r_out = (double)r + 0.55;
    double r_in2 = r_in * r_in;
    double r_out2 = r_out * r_out;
    for (int dy = -(r + 1); dy <= r + 1; dy++) {
        for (int dx = -(r + 1); dx <= r + 1; dx++) {
            int d2 = dx * dx + dy * dy;
            if ((double)d2 <= r_in2) {
                put_px32(cx + dx, cy + dy, col);
            } else if ((double)d2 < r_out2) {
                double dist = sqrt((double)d2);
                int a = (int)((r_out - dist) * 255.0 + 0.5);
                if (a > 0) {
                    if (a > 255) a = 255;
                    blend_px32(cx + dx, cy + dy, col, (uint8_t)a);
                }
            }
        }
    }
}

static void draw_line32(int x0, int y0, int x1, int y1, uint32_t col) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px32(x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int draw_glyph32(int x, int y, uint32_t cp, uint32_t fg, int scale) {
    int style = (scale >= 2) ? IPO_FONT_BOLD : IPO_FONT_REGULAR;
    int h = (scale >= 2) ? (14 * scale > 32 ? 32 : 14 * scale) : ui_font_px();
    return ipo_font_draw_glyph_rgb32(s_fb32, s_gui_w, 0, 0, s_gui_w, s_gui_h,
                                     x, y, style, h, cp, fg & 0x00FFFFFFu);
}

static void draw_text_styled32(int x, int y, const char *utf8, uint32_t fg,
                               int max_w, int style, int font_px) {
    if (!utf8 || max_w <= 0) return;
    int clip_x2 = x + max_w;
    if (clip_x2 > s_gui_w) clip_x2 = s_gui_w;
    uint32_t rgb = fg & 0x00FFFFFFu;

    int non_ascii = 0;
    for (const unsigned char *s = (const unsigned char *)utf8; *s; s++) {
        if (*s >= 0x80u) { non_ascii = 1; break; }
    }
    if (!non_ascii) {
        const char *p = utf8;
        int cx = x;
        while (*p && cx < clip_x2) {
            uint32_t cp = (unsigned char)*p++;
            if (cp == ' ') {
                int adv = ipo_font_measure_glyph(style, font_px, ' ');
                cx += (adv > 0) ? adv : (font_px / 3 + 1);
                continue;
            }
            cx += ipo_font_draw_glyph_rgb32(s_fb32, s_gui_w, 0, 0, clip_x2, s_gui_h,
                                            cx, y, style, font_px, cp, rgb);
        }
        return;
    }

    ipo_shaped_glyph_t stack_out[128];
    int total = ipo_font_shape_utf8(utf8, stack_out, 128);
    ipo_shaped_glyph_t *arr = stack_out;
    ipo_shaped_glyph_t *heap_arr = NULL;
    if (total > 128) {
        heap_arr = (ipo_shaped_glyph_t *)malloc((size_t)total * sizeof(ipo_shaped_glyph_t));
        if (heap_arr) {
            ipo_font_shape_utf8(utf8, heap_arr, total);
            arr = heap_arr;
        } else {
            total = 128;
        }
    }
    int cx = x, last_x = x, last_w = font_px / 2;
    for (int i = 0; i < total && cx < clip_x2; i++) {
        uint32_t cp = arr[i].cp;
        if (arr[i].flags & IPO_SHAPED_ZEROWIDTH) continue;
        if (arr[i].flags & IPO_SHAPED_OVERSTRIKE) {
            ipo_font_draw_overstrike_rgb32(s_fb32, s_gui_w, 0, 0, clip_x2, s_gui_h,
                                           last_x, last_w, y, style, font_px, cp, rgb);
            continue;
        }
        if (cp == ' ') {
            int adv = ipo_font_measure_glyph(style, font_px, ' ');
            int sp = (adv > 0) ? adv : (font_px / 3 + 1);
            last_x = cx; last_w = sp; cx += sp;
            continue;
        }
        int adv = ipo_font_draw_glyph_rgb32(s_fb32, s_gui_w, 0, 0, clip_x2, s_gui_h,
                                            cx, y, style, font_px, cp, rgb);
        last_x = cx; last_w = adv; cx += adv;
    }
    if (heap_arr) free(heap_arr);
}

static void draw_text_clip32(int x, int y, const char *utf8, uint32_t fg, int max_w, int scale) {
    int style = (scale >= 2) ? IPO_FONT_BOLD : IPO_FONT_REGULAR;
    int h = (scale >= 2) ? (14 * scale > 32 ? 32 : 14 * scale) : ui_font_px();
    draw_text_styled32(x, y, utf8, fg, max_w, style, h);
}

static void draw_text_ellipsis32(int x, int y, const char *utf8, uint32_t fg, int max_w) {
    if (!utf8 || max_w <= 0) return;
    int fh = ui_font_px();
    int total_w = ipo_font_text_width(IPO_FONT_REGULAR, fh, utf8);
    if (total_w <= max_w) {
        draw_text_styled32(x, y, utf8, fg, max_w, IPO_FONT_REGULAR, fh);
        return;
    }
    int dots_w = ui_glyph_adv(0x2026u);
    int limit_w = max_w - dots_w;
    if (limit_w < 0) limit_w = 0;
    const char *p = utf8;
    int cx = x;
    uint32_t rgb = fg & 0x00FFFFFFu;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (!cp) break;
        int adv = ui_glyph_adv(cp);
        if ((cx - x) + adv > limit_w) break;
        if (cp != ' ') {
            ipo_font_draw_glyph_rgb32(s_fb32, s_gui_w, 0, 0, s_gui_w, s_gui_h,
                                      cx, y, IPO_FONT_REGULAR, fh, cp, rgb);
        }
        cx += adv;
    }
    if (dots_w <= max_w) {
        ipo_font_draw_glyph_rgb32(s_fb32, s_gui_w, 0, 0, x + max_w, s_gui_h,
                                  cx, y, IPO_FONT_REGULAR, fh, 0x2026u, rgb);
    }
}

static void draw_text_scrolled32(int x, int y, const char *buf, int len,
                                 int cursor, int *scroll_col, bool focused, bool sel_all,
                                 uint32_t fg, uint32_t dim_fg, const char *placeholder,
                                 int max_w, int bar_y, int bar_h) {
    if (max_w < 8) return;
    int caret_h = ui_char_h() + 2;
    int caret_y = y - 1;
    if (caret_y < bar_y + 1) caret_y = bar_y + 1;
    if (caret_y + caret_h > bar_y + bar_h - 1) caret_h = (bar_y + bar_h - 1) - caret_y;
    if (caret_h < 2) caret_h = 2;

    if (len <= 0) {
        if (scroll_col) *scroll_col = 0;
        if (placeholder && placeholder[0]) {
            draw_text_clip32(x, y, placeholder, dim_fg, max_w, 1);
        }
        if (focused && ((gui_millis() / 450u) & 1u) == 0) {
            fill_rect32(x, caret_y, ui_is_compact_font() ? 1 : 2, caret_h, COL_ACCENT_CYAN);
        }
        return;
    }

    int sc = scroll_col ? *scroll_col : 0;
    int total_cols = utf8_total_cols(buf, len);
    int avail_w = max_w - 6;
    if (avail_w < 8) avail_w = 8;
    if (focused) {
        textbox_sync_scroll(buf, len, cursor, &sc, max_w);
        if (scroll_col) *scroll_col = sc;
    } else {
        if (sc > total_cols) sc = total_cols;
        if (sc < 0) sc = 0;
        while (sc > 0 && ui_substr_w(buf, len, sc - 1, total_cols) <= avail_w) sc--;
        if (scroll_col) *scroll_col = sc;
    }

    int start_byte = utf8_col_to_byte(buf, len, sc);
    int shown_w = ui_substr_w(buf, len, sc, total_cols);
    if (shown_w > avail_w) shown_w = avail_w;

    if (sel_all && focused && shown_w > 0) {
        fill_rect32(x - 1, caret_y, shown_w + 2, caret_h, COL_ACCENT_BLUE);
    }

    draw_text_styled32(x, y, buf + start_byte, fg, max_w - 2, IPO_FONT_REGULAR, ui_font_px());

    /* Subtle edge markers when text extends beyond left/right scroll window */
    if (sc > 0) {
        int mid_y = bar_y + bar_h / 2;
        put_px32(x - 3, mid_y, COL_ACCENT_CYAN);
        put_px32(x - 2, mid_y - 1, COL_ACCENT_CYAN);
        put_px32(x - 2, mid_y + 1, COL_ACCENT_CYAN);
    }
    if (ui_substr_w(buf, len, sc, total_cols) > avail_w) {
        int mid_y = bar_y + bar_h / 2;
        int rx = x + max_w - 2;
        put_px32(rx + 1, mid_y, COL_ACCENT_CYAN);
        put_px32(rx, mid_y - 1, COL_ACCENT_CYAN);
        put_px32(rx, mid_y + 1, COL_ACCENT_CYAN);
    }

    if (focused && ((gui_millis() / 450u) & 1u) == 0) {
        int cur_col = utf8_byte_to_col(buf, cursor);
        if (cur_col >= sc) {
            int cx = x + ui_substr_w(buf, len, sc, cur_col);
            if (cx <= x + max_w - 2) {
                fill_rect32(cx, caret_y, ui_is_compact_font() ? 1 : 2, caret_h, COL_ACCENT_CYAN);
            }
        }
    }
}

/* Vector Chrome Icons */
static void draw_icon_back(int cx, int cy, uint32_t col) {
    int r = ui_is_compact_font() ? 3 : 4;
    draw_line32(cx - r, cy, cx + r, cy, col);
    draw_line32(cx - r, cy, cx, cy - r, col);
    draw_line32(cx - r, cy, cx, cy + r, col);
}

static void draw_icon_fwd(int cx, int cy, uint32_t col) {
    int r = ui_is_compact_font() ? 3 : 4;
    draw_line32(cx - r, cy, cx + r, cy, col);
    draw_line32(cx + r, cy, cx, cy - r, col);
    draw_line32(cx + r, cy, cx, cy + r, col);
}

static void draw_icon_reload(int cx, int cy, uint32_t col) {
    double rad_r = ui_is_compact_font() ? 3.5 : 5.0;
    int ar = ui_is_compact_font() ? 3 : 5;
    for (int a = 30; a <= 315; a += 14) {
        double rad = (double)a * 3.1415926 / 180.0;
        int px = cx + (int)(cos(rad) * rad_r);
        int py = cy + (int)(sin(rad) * rad_r);
        put_px32(px, py, col);
    }
    draw_line32(cx + (ar - 2), cy - ar, cx + ar, cy - (ar - 2), col);
    draw_line32(cx + ar, cy - ar, cx + ar, cy - (ar - 2), col);
}

static void draw_icon_home(int cx, int cy, uint32_t col) {
    int r = ui_is_compact_font() ? 3 : 5;
    int w = ui_is_compact_font() ? 2 : 4;
    draw_line32(cx, cy - r, cx - r, cy, col);
    draw_line32(cx, cy - r, cx + r, cy, col);
    draw_line32(cx - w, cy, cx - w, cy + r, col);
    draw_line32(cx + w, cy, cx + w, cy + r, col);
    draw_line32(cx - w, cy + r, cx + w, cy + r, col);
}

static void draw_icon_lock(int cx, int cy, uint32_t col) {
    if (ui_is_compact_font()) {
        fill_rect32(cx - 2, cy - 1, 5, 4, col);
        draw_line32(cx - 1, cy - 2, cx - 1, cy - 3, col);
        draw_line32(cx + 1, cy - 2, cx + 1, cy - 3, col);
        put_px32(cx, cy - 3, col);
        return;
    }
    stroke_rect32(cx - 3, cy - 1, 7, 6, col);
    fill_rect32(cx - 2, cy, 5, 4, col);
    draw_line32(cx - 2, cy - 2, cx - 2, cy - 4, col);
    draw_line32(cx + 2, cy - 2, cx + 2, cy - 4, col);
    draw_line32(cx - 1, cy - 5, cx + 1, cy - 5, col);
}

static void draw_icon_star(int cx, int cy, uint32_t col, bool filled) {
    if (ui_is_compact_font()) {
        draw_line32(cx, cy - 3, cx + 2, cy + 3, col);
        draw_line32(cx, cy - 3, cx - 2, cy + 3, col);
        draw_line32(cx - 3, cy - 1, cx + 3, cy - 1, col);
        draw_line32(cx - 3, cy - 1, cx + 2, cy + 3, col);
        draw_line32(cx + 3, cy - 1, cx - 2, cy + 3, col);
        if (filled) fill_rect32(cx - 1, cy - 1, 3, 3, col);
        return;
    }
    draw_line32(cx, cy - 5, cx + 4, cy + 4, col);
    draw_line32(cx, cy - 5, cx - 4, cy + 4, col);
    draw_line32(cx - 5, cy - 1, cx + 5, cy - 1, col);
    draw_line32(cx - 5, cy - 1, cx + 4, cy + 4, col);
    draw_line32(cx + 5, cy - 1, cx - 4, cy + 4, col);
    if (filled) fill_circle32(cx, cy, 2, col);
}

static void draw_icon_download(int cx, int cy, uint32_t col) {
    if (ui_is_compact_font()) {
        draw_line32(cx, cy - 3, cx, cy + 1, col);
        draw_line32(cx - 2, cy - 1, cx, cy + 1, col);
        draw_line32(cx + 2, cy - 1, cx, cy + 1, col);
        draw_line32(cx - 3, cy + 3, cx + 3, cy + 3, col);
        return;
    }
    draw_line32(cx, cy - 5, cx, cy + 2, col);
    draw_line32(cx - 3, cy - 1, cx, cy + 2, col);
    draw_line32(cx + 3, cy - 1, cx, cy + 2, col);
    draw_line32(cx - 5, cy + 4, cx + 5, cy + 4, col);
}

static void draw_icon_play(int cx, int cy, int r, uint32_t col) {
    for (int dx = -r; dx <= r; dx++) {
        int half = ((r - dx) * r) / (2 * r + 1);
        for (int dy = -half; dy <= half; dy++) {
            put_px32(cx + dx, cy + dy, col);
        }
    }
}

static void draw_icon_pause(int cx, int cy, int r, uint32_t col) {
    fill_rect32(cx - r, cy - r, 3, r * 2 + 1, col);
    fill_rect32(cx + 1, cy - r, 3, r * 2 + 1, col);
}

static void draw_icon_close(int cx, int cy, int r, uint32_t col) {
    draw_line32(cx - r, cy - r, cx + r, cy + r, col);
    draw_line32(cx - r, cy + r, cx + r, cy - r, col);
}

/* =========================================================================
 * Real Progressive MP4 / H.264 / AAC Media Player Helpers
 * ========================================================================= */
static mp4_player_t *ensure_mp4_player(void) {
    if (!s_mp4_player) {
        s_mp4_player = mp4_player_create();
    }
    return s_mp4_player;
}

static void video_stop_audio(void) {
    if (s_mp4_player) {
        mp4_player_stop(s_mp4_player);
    }
}

static void video_start_audio(void) {
    mp4_player_t *mp = ensure_mp4_player();
    if (!mp || !s_tabs || s_active_tab < 0 || s_active_tab >= s_tabs_cap) return;
    ipo_tab_t *tab = &s_tabs[s_active_tab];
    if (!tab->used || !tab->is_video) return;
    mp4_player_set_muted(mp, s_vid_muted);
    mp4_player_open(mp, tab->video_url);
}

/* =========================================================================
 * Non-Blocking HTTP / HTTPS File Downloader Engine
 * ========================================================================= */
static int parse_http_url_alloc(const char *url, bool *out_https, char **out_host,
                                int *out_port, char **out_path) {
    if (!url || !*url || !out_https || !out_host || !out_port || !out_path) return -1;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        *out_https = true;
        *out_port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        *out_https = false;
        *out_port = 80;
        p += 7;
    } else {
        return -1;
    }
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
    if (colon && (!slash || colon < slash)) {
        host_len = (size_t)(colon - p);
        int parsed_port = atoi(colon + 1);
        const int max_tcp_port = (int)((1u << (sizeof(uint16_t) * 8u)) - 1u);
        if (parsed_port > 0 && parsed_port <= max_tcp_port) *out_port = parsed_port;
    }
    if (host_len == 0) return -1;
    if (!gui_set_strn(out_host, p, host_len)) return -1;
    if (!gui_set_str(out_path, (slash && *slash) ? slash : "/")) return -1;
    return 0;
}

static bool dl_append_hdr_bytes(dl_slot_t *dl, const uint8_t *data, size_t len) {
    if (!dl) return false;
    if (len == 0) return true;
    if (dl->hdr_len > SIZE_MAX - len - 1u) return false;
    size_t need = dl->hdr_len + len + 1u;
    if (!gui_dyn_ensure(&dl->hdr_buf, &dl->hdr_cap, need)) return false;
    memcpy(dl->hdr_buf + dl->hdr_len, data, len);
    dl->hdr_len += len;
    dl->hdr_buf[dl->hdr_len] = '\0';
    return true;
}

static int dl_hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char *derive_download_filename_alloc(const char *url) {
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *path_end = p;
    while (*path_end && *path_end != '?' && *path_end != '#') path_end++;

    const char *slash = NULL;
    const char *last = NULL;
    for (const char *s = p; s < path_end; s++) {
        if (*s == '/') {
            if (!slash) slash = s;
            last = s;
        }
    }

    if (last && last > p && last + 1 < path_end) {
        size_t raw_len = (size_t)(path_end - (last + 1));
        char *out = (char *)malloc(raw_len * 2u + 1u);
        if (out) {
            size_t oi = 0;
            const char *q = last + 1;
            while (q < path_end) {
                unsigned char uc = (unsigned char)*q++;
                if (uc == '%' && q + 1 < path_end) {
                    int hi = dl_hex_val(q[0]);
                    int lo = dl_hex_val(q[1]);
                    if (hi >= 0 && lo >= 0) {
                        uc = (unsigned char)((hi << 4) | lo);
                        q += 2;
                    }
                }
                if (uc < 0x20 || uc == 0x7F) continue;
                if (uc == '/') {
                    out[oi++] = '\\';
                    out[oi++] = '/';
                } else {
                    out[oi++] = (char)uc;
                }
            }
            out[oi] = '\0';
            if (oi > 0 && strcmp(out, ".") != 0 && strcmp(out, "..") != 0) {
                return out;
            }
            free(out);
        }
    }

    /* Derive from host without length truncation or arbitrary character stripping */
    size_t hl = slash ? (size_t)(slash - p) : (size_t)(path_end - p);
    const char *src = hl > 0 ? p : "download";
    size_t slen = hl > 0 ? hl : 8u;
    char *out = (char *)malloc(slen * 2u + 6u);
    if (!out) return NULL;
    size_t oi = 0;
    for (size_t i = 0; i < slen; i++) {
        unsigned char uc = (unsigned char)src[i];
        if (uc < 0x20 || uc == 0x7F) continue;
        if (uc == '/') {
            out[oi++] = '\\';
            out[oi++] = '/';
        } else {
            out[oi++] = (char)uc;
        }
    }
    if (oi == 0) {
        memcpy(out, "download", 8u);
        oi = 8u;
    }
    memcpy(out + oi, ".html", 6u);
    return out;
}

static void dl_slot_close_conn(dl_slot_t *dl) {
    if (dl->ssl) {
        SSL_shutdown(dl->ssl);
        SSL_free(dl->ssl);
        dl->ssl = NULL;
    }
    if (dl->ctx) {
        SSL_CTX_free(dl->ctx);
        dl->ctx = NULL;
    }
    if (dl->sock >= 0) {
        close(dl->sock);
        dl->sock = -1;
    }
    if (dl->fd >= 0) {
        close(dl->fd);
        dl->fd = -1;
    }
}

static void dl_slot_feed(dl_slot_t *dl, const uint8_t *data, size_t len) {
    size_t i = 0;
    if (!dl->chunked) {
        if (dl->fd >= 0 && len > 0) {
            ssize_t w = write(dl->fd, data, len);
            if (w > 0) dl->downloaded_bytes += (uint64_t)w;
        }
        return;
    }
    while (i < len && dl->chunk_state != 5) {
        uint8_t c = data[i];
        switch (dl->chunk_state) {
            case 0:
                if (c == '\r') dl->chunk_state = 1;
                else if (c == '\n') {
                    if (dl->chunk_line) dl->chunk_line[dl->chunk_line_len] = '\0';
                    dl->chunk_rem = dl->chunk_line ? (size_t)strtoul(dl->chunk_line, NULL, 16) : 0u;
                    dl->chunk_line_len = 0;
                    dl->chunk_state = (dl->chunk_rem == 0) ? 5 : 2;
                } else if (c != ';') {
                    if (gui_dyn_ensure(&dl->chunk_line, &dl->chunk_line_cap, dl->chunk_line_len + 2u)) {
                        dl->chunk_line[dl->chunk_line_len++] = (char)c;
                    }
                }
                i++;
                break;
            case 1:
                if (c == '\n') {
                    if (dl->chunk_line) dl->chunk_line[dl->chunk_line_len] = '\0';
                    dl->chunk_rem = dl->chunk_line ? (size_t)strtoul(dl->chunk_line, NULL, 16) : 0u;
                    dl->chunk_line_len = 0;
                    dl->chunk_state = (dl->chunk_rem == 0) ? 5 : 2;
                }
                i++;
                break;
            case 2: {
                size_t avail = len - i;
                size_t take = avail < dl->chunk_rem ? avail : dl->chunk_rem;
                if (take > 0 && dl->fd >= 0) {
                    ssize_t w = write(dl->fd, data + i, take);
                    if (w > 0) dl->downloaded_bytes += (uint64_t)w;
                }
                dl->chunk_rem -= take;
                i += take;
                if (dl->chunk_rem == 0) dl->chunk_state = 3;
                break;
            }
            case 3:
                if (c == '\r') dl->chunk_state = 4;
                else if (c == '\n') dl->chunk_state = 0;
                i++;
                break;
            case 4:
                dl->chunk_state = 0;
                i++;
                break;
            default:
                i = len;
                break;
        }
    }
}

static bool dl_begin_request(dl_slot_t *dl, const char *target_url) {
    dl_slot_close_conn(dl);
    if (!gui_set_str(&dl->cur_url, target_url)) {
        gui_set_str(&dl->err_msg, "Out of memory");
        dl->state = DL_ERROR;
        return false;
    }
    if (!dl_slot_record_url(dl, gui_str(dl->cur_url))) {
        gui_set_str(&dl->err_msg, "Out of memory");
        dl->state = DL_ERROR;
        return false;
    }
    if (parse_http_url_alloc(gui_str(dl->cur_url), &dl->is_https, &dl->host,
                             &dl->port, &dl->path) != 0) {
        gui_set_str(&dl->err_msg, "Invalid URL");
        dl->state = DL_ERROR;
        return false;
    }

    struct hostent *he = gethostbyname(gui_str(dl->host));
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        gui_set_str(&dl->err_msg, "DNS failed");
        dl->state = DL_ERROR;
        return false;
    }

    dl->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (dl->sock < 0) {
        gui_set_str(&dl->err_msg, "Socket error");
        dl->state = DL_ERROR;
        return false;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)dl->port);
    memcpy(&sin.sin_addr, he->h_addr_list[0], sizeof(sin.sin_addr));

    if (connect(dl->sock, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        dl_slot_close_conn(dl);
        gui_set_str(&dl->err_msg, "Connect failed");
        dl->state = DL_ERROR;
        return false;
    }

    int fl = fcntl(dl->sock, F_GETFL, 0);
    if (fl >= 0) fcntl(dl->sock, F_SETFL, fl | O_NONBLOCK);

    dl->hdr_len = 0;
    if (dl->hdr_buf) dl->hdr_buf[0] = '\0';
    dl->headers_done = false;
    dl->status_code = 0;
    dl->chunked = false;
    dl->chunk_state = 0;
    dl->chunk_rem = 0;
    dl->chunk_line_len = 0;
    dl->content_length = -1;

    if (dl->is_https) {
        dl->ctx = SSL_CTX_new(TLS_client_method());
        if (!dl->ctx) {
            dl_slot_close_conn(dl);
            gui_set_str(&dl->err_msg, "SSL ctx failed");
            dl->state = DL_ERROR;
            return false;
        }
        dl->ssl = SSL_new(dl->ctx);
        if (!dl->ssl) {
            dl_slot_close_conn(dl);
            gui_set_str(&dl->err_msg, "SSL new failed");
            dl->state = DL_ERROR;
            return false;
        }
        SSL_set_fd(dl->ssl, dl->sock);
        SSL_set_tlsext_host_name(dl->ssl, gui_str(dl->host));
        dl->state = DL_TLS_HANDSHAKE;
    } else {
        char *req = gui_fmt_alloc("GET %s HTTP/1.1\r\nHost: %s\r\n"
                                  "User-Agent: Mozilla/5.0 (IPO_OS; Links/2.22)\r\n"
                                  "Accept: */*\r\nAccept-Encoding: identity\r\n"
                                  "Connection: close\r\n\r\n",
                                  gui_str(dl->path), gui_str(dl->host));
        if (!req) {
            dl_slot_close_conn(dl);
            gui_set_str(&dl->err_msg, "Out of memory");
            dl->state = DL_ERROR;
            return false;
        }
        send(dl->sock, req, strlen(req), 0);
        free(req);
        dl->state = DL_READING;
    }
    return true;
}

static void gui_start_download(const char *raw_url) {
    if (!raw_url || !*raw_url) return;
    char *norm_url = (strncmp(raw_url, "http://", 7) != 0 && strncmp(raw_url, "https://", 8) != 0)
                     ? gui_fmt_alloc("https://%s", raw_url)
                     : gui_fmt_alloc("%s", raw_url);
    if (!norm_url) return;

    int slot = -1;
    for (int i = 0; i < s_dls_cap; i++) {
        if (!s_dls[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        int new_cap = s_dls_cap ? (s_dls_cap * 2) : 8;
        if (new_cap <= s_dls_cap || (size_t)new_cap > SIZE_MAX / sizeof(dl_slot_t)) {
            free(norm_url);
            return;
        }
        dl_slot_t *nd = (dl_slot_t *)realloc(s_dls, (size_t)new_cap * sizeof(dl_slot_t));
        if (!nd) {
            free(norm_url);
            return;
        }
        memset(nd + s_dls_cap, 0, (size_t)(new_cap - s_dls_cap) * sizeof(dl_slot_t));
        slot = s_dls_cap;
        s_dls = nd;
        s_dls_cap = new_cap;
    }

    dl_slot_t *dl = &s_dls[slot];
    dl_slot_close_conn(dl);
    dl_slot_free_strings(dl);
    memset(dl, 0, sizeof(*dl));
    dl->used = true;
    dl->sock = -1;
    dl->fd = -1;
    dl->start_ms = gui_millis();
    if (!gui_set_str(&dl->url, norm_url)) {
        free(norm_url);
        dl->used = false;
        return;
    }
    dl->filename = derive_download_filename_alloc(norm_url);
    free(norm_url);
    if (!dl->filename) {
        dl_slot_free_strings(dl);
        dl->used = false;
        return;
    }
    mkdir("/data", 0755);
    size_t flen = strlen(dl->filename);
    dl->dest = (char *)malloc(6u + flen + 1u);
    if (!dl->dest) {
        dl_slot_free_strings(dl);
        dl->used = false;
        return;
    }
    memcpy(dl->dest, "/data/", 6u);
    memcpy(dl->dest + 6u, dl->filename, flen + 1u);
    dl->state = DL_CONNECTING;

    s_overlay = OVL_DOWNLOADS;
    s_dirty_chrome = true;
    s_dirty_overlay = true;
    gui_set_fmt(&s_status_msg, "Downloading -> %s", dl->dest);
}

static void dl_tick_all(void) {
    bool changed = false;
    for (int i = 0; i < s_dls_cap; i++) {
        dl_slot_t *dl = &s_dls[i];
        if (!dl->used) continue;
        if (dl->state == DL_CONNECTING) {
            dl_begin_request(dl, gui_str(dl->url));
            changed = true;
            continue;
        }
        if (dl->state == DL_TLS_HANDSHAKE) {
            int hs = SSL_connect(dl->ssl);
            if (hs == 1) {
                char *req = gui_fmt_alloc("GET %s HTTP/1.1\r\nHost: %s\r\n"
                                          "User-Agent: Mozilla/5.0 (IPO_OS; Links/2.22)\r\n"
                                          "Accept: */*\r\nAccept-Encoding: identity\r\n"
                                          "Connection: close\r\n\r\n",
                                          gui_str(dl->path), gui_str(dl->host));
                if (!req) {
                    dl_slot_close_conn(dl);
                    gui_set_str(&dl->err_msg, "Out of memory");
                    dl->state = DL_ERROR;
                } else {
                    SSL_write(dl->ssl, req, (int)strlen(req));
                    free(req);
                    dl->state = DL_READING;
                }
                changed = true;
            } else {
                int err = SSL_get_error(dl->ssl, hs);
                if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                    dl_slot_close_conn(dl);
                    gui_set_str(&dl->err_msg, "TLS error");
                    dl->state = DL_ERROR;
                    changed = true;
                }
            }
            continue;
        }
        if (dl->state == DL_READING) {
            uint8_t buf[4096];
            for (int iter = 0; iter < 32 && dl->state == DL_READING; iter++) {
                int r;
                if (dl->is_https) {
                    r = SSL_read(dl->ssl, buf, sizeof(buf));
                    if (r < 0) {
                        int err = SSL_get_error(dl->ssl, r);
                        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) break;
                        dl_slot_close_conn(dl);
                        dl->end_ms = gui_millis();
                        if (dl->headers_done && dl->downloaded_bytes > 0) {
                            dl->state = DL_DONE;
                        } else {
                            gui_set_str(&dl->err_msg, "Read error");
                            dl->state = DL_ERROR;
                        }
                        changed = true;
                        break;
                    }
                } else {
                    ssize_t rr = recv(dl->sock, buf, sizeof(buf), 0);
                    if (rr < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        dl_slot_close_conn(dl);
                        dl->end_ms = gui_millis();
                        if (dl->headers_done && dl->downloaded_bytes > 0) {
                            dl->state = DL_DONE;
                        } else {
                            gui_set_str(&dl->err_msg, "Recv error");
                            dl->state = DL_ERROR;
                        }
                        changed = true;
                        break;
                    }
                    r = (int)rr;
                }
                if (r == 0) {
                    dl_slot_close_conn(dl);
                    dl->end_ms = gui_millis();
                    if (dl->headers_done) {
                        dl->state = DL_DONE;
                        gui_set_fmt(&s_status_msg,
                                    "Saved %s (%lu bytes)",
                                    gui_str(dl->dest), (unsigned long)dl->downloaded_bytes);
                    } else {
                        gui_set_str(&dl->err_msg, "Empty response");
                        dl->state = DL_ERROR;
                    }
                    changed = true;
                    break;
                }
                changed = true;
                if (!dl->headers_done) {
                    if (!dl_append_hdr_bytes(dl, buf, (size_t)r)) {
                        dl_slot_close_conn(dl);
                        gui_set_str(&dl->err_msg, "Out of memory");
                        dl->state = DL_ERROR;
                        break;
                    }
                    char *sep = dl->hdr_buf ? strstr(dl->hdr_buf, "\r\n\r\n") : NULL;
                    if (sep) {
                        *sep = '\0';
                        dl->headers_done = true;
                        const char *sp = strchr(dl->hdr_buf, ' ');
                        dl->status_code = sp ? atoi(sp + 1) : 200;
                        if (dl->status_code == 301 || dl->status_code == 302 ||
                            dl->status_code == 303 || dl->status_code == 307 ||
                            dl->status_code == 308) {
                            const char *loc = ci_strstr_local(dl->hdr_buf, "\nLocation:");
                            if (loc) {
                                loc = strchr(loc, ':') + 1;
                                while (*loc == ' ' || *loc == '\t') loc++;
                                const char *eol = strpbrk(loc, "\r\n");
                                size_t ll = eol ? (size_t)(eol - loc) : strlen(loc);
                                char *next_url = NULL;
                                if (ll > 0) {
                                    if ((ll >= 7 && strncmp(loc, "http://", 7) == 0) ||
                                        (ll >= 8 && strncmp(loc, "https://", 8) == 0)) {
                                        gui_set_strn(&next_url, loc, ll);
                                    } else if (loc[0] == '/') {
                                        next_url = gui_fmt_alloc("%s://%s%.*s",
                                                                 dl->is_https ? "https" : "http",
                                                                 gui_str(dl->host), (int)ll, loc);
                                    }
                                }
                                if (next_url && next_url[0] && !dl_slot_seen_url(dl, next_url)) {
                                    dl->redirects++;
                                    dl_begin_request(dl, next_url);
                                    free(next_url);
                                    break;
                                }
                                free(next_url);
                            }
                        }

                        dl->chunked = (ci_strstr_local(dl->hdr_buf, "Transfer-Encoding: chunked") != NULL) ||
                                      (ci_strstr_local(dl->hdr_buf, "Transfer-Encoding:chunked") != NULL);
                        const char *cl = ci_strstr_local(dl->hdr_buf, "Content-Length:");
                        if (cl) {
                            cl = strchr(cl, ':') + 1;
                            while (*cl == ' ' || *cl == '\t') cl++;
                            dl->content_length = (int64_t)strtoll(cl, NULL, 10);
                        }
                        dl->fd = dl->dest ? open(dl->dest, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
                        if (dl->fd < 0) {
                            dl_slot_close_conn(dl);
                            gui_set_str(&dl->err_msg, "File create failed");
                            dl->state = DL_ERROR;
                            break;
                        }
                        size_t consumed = (size_t)((sep + 4) - dl->hdr_buf);
                        if (dl->hdr_len > consumed) {
                            dl_slot_feed(dl, (const uint8_t *)(sep + 4), dl->hdr_len - consumed);
                        }
                    }
                } else {
                    dl_slot_feed(dl, buf, (size_t)r);
                }
                if ((dl->chunked && dl->chunk_state == 5) ||
                    (!dl->chunked && dl->content_length >= 0 &&
                     (int64_t)dl->downloaded_bytes >= dl->content_length)) {
                    dl_slot_close_conn(dl);
                    dl->end_ms = gui_millis();
                    dl->state = DL_DONE;
                    gui_set_fmt(&s_status_msg,
                                "Saved %s (%lu bytes)",
                                gui_str(dl->dest), (unsigned long)dl->downloaded_bytes);
                    changed = true;
                    break;
                }
            }
        }
    }
    if (changed) {
        s_dirty_chrome = true;
        if (s_overlay == OVL_DOWNLOADS) s_dirty_overlay = true;
    }
}

/* =========================================================================
 * Navigation, Tabs, Bookmarks, History, Autocomplete & Zoom
 * ========================================================================= */
static void record_history_visit(const char *url, const char *title) {
    if (!url || !*url || strcmp(url, "about:home") == 0 || strcmp(url, "links://home") == 0) return;
    /* Update existing entry if already at top of history */
    if (s_history_count > 0 && s_history[s_history_count - 1].url &&
        strcmp(s_history[s_history_count - 1].url, url) == 0) {
        if (title && *title) {
            gui_set_str(&s_history[s_history_count - 1].title, title);
        }
        s_history[s_history_count - 1].visit_ms = gui_millis();
        return;
    }
    if (s_history_count >= s_history_cap) {
        int new_cap = (s_history_cap > 0) ? (s_history_cap * 2) : 16;
        if (new_cap <= s_history_cap || (size_t)new_cap > SIZE_MAX / sizeof(ipo_hist_entry_t)) return;
        ipo_hist_entry_t *nh = (ipo_hist_entry_t *)realloc(s_history, (size_t)new_cap * sizeof(ipo_hist_entry_t));
        if (!nh) return;
        memset(nh + s_history_cap, 0, (size_t)(new_cap - s_history_cap) * sizeof(ipo_hist_entry_t));
        s_history = nh;
        s_history_cap = new_cap;
    }
    ipo_hist_entry_t *e = &s_history[s_history_count];
    e->url = NULL;
    e->title = NULL;
    if (!gui_set_str(&e->url, url) ||
        !gui_set_str(&e->title, (title && *title) ? title : url)) {
        free(e->url);
        free(e->title);
        e->url = NULL;
        e->title = NULL;
        return;
    }
    e->visit_ms = gui_millis();
    s_history_count++;
}

static void rebuild_omnibox_autocomplete(void) {
    ac_clear_items();
    const char *q = gui_str(s_omnibox_text);
    while (*q == ' ') q++;
    if (!*q) {
        s_ac_sel = -1;
        return;
    }
    /* 1. Match bookmarks */
    for (int i = 0; i < s_bookmark_count; i++) {
        if (ci_strstr_local(gui_str(s_bookmarks[i].label), q) ||
            ci_strstr_local(gui_str(s_bookmarks[i].url), q)) {
            if (!ac_ensure_cap(s_ac_count + 1)) break;
            ipo_ac_item_t *it = &s_ac_items[s_ac_count++];
            gui_set_str(&it->label, gui_str(s_bookmarks[i].label));
            gui_set_str(&it->url, gui_str(s_bookmarks[i].url));
            it->is_bookmark = true;
            it->is_search = false;
        }
    }
    /* 2. Match recent history */
    for (int i = s_history_count - 1; i >= 0; i--) {
        if (ci_strstr_local(gui_str(s_history[i].title), q) ||
            ci_strstr_local(gui_str(s_history[i].url), q)) {
            bool dup = false;
            for (int k = 0; k < s_ac_count; k++) {
                if (s_ac_items[k].url && s_history[i].url &&
                    strcmp(s_ac_items[k].url, s_history[i].url) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                if (!ac_ensure_cap(s_ac_count + 1)) break;
                ipo_ac_item_t *it = &s_ac_items[s_ac_count++];
                gui_set_str(&it->label, gui_str(s_history[i].title));
                gui_set_str(&it->url, gui_str(s_history[i].url));
                it->is_bookmark = false;
                it->is_search = false;
            }
        }
    }
    /* 3. Always offer Google Search or direct URL as final option */
    if (ac_ensure_cap(s_ac_count + 1)) {
        ipo_ac_item_t *it = &s_ac_items[s_ac_count++];
        gui_set_fmt(&it->label, "Search Google for \"%s\"", q);
        gui_set_str(&it->url, q);
        it->is_bookmark = false;
        it->is_search = true;
    }
    if (s_ac_sel >= s_ac_count) s_ac_sel = s_ac_count - 1;
}

static bool is_tab_loading(int idx) {
    if (idx < 0 || idx >= s_tabs_cap || !s_tabs || !s_tabs[idx].used) return false;
    if (s_tabs[idx].is_home || s_tabs[idx].is_video || !s_tabs[idx].dev) return false;
    struct session *ses = dev_to_session(s_tabs[idx].dev);
    if (!ses) return false;
    if (ses->rq && ses->rq->state >= 0) return true;
    if (ses->screen && ses->screen->rq && ses->screen->rq->state >= 0) return true;
    if (ses->screen && !ses->screen->done && ses->screen->rq) return true;
    return false;
}

static bool can_tab_go_back(int idx) {
    if (idx < 0 || idx >= s_tabs_cap || !s_tabs || !s_tabs[idx].used) return false;
    if (s_tabs[idx].is_video) return true;
    if (s_tabs[idx].is_home) return false;
    struct session *ses = dev_to_session(s_tabs[idx].dev);
    if (!ses) return true;
    return true; /* Can always go back to previous history or Start Page */
}

static bool can_tab_go_forward(int idx) {
    if (idx < 0 || idx >= s_tabs_cap || !s_tabs || !s_tabs[idx].used) return false;
    if (s_tabs[idx].is_home || s_tabs[idx].is_video || !s_tabs[idx].dev) return false;
    struct session *ses = dev_to_session(s_tabs[idx].dev);
    if (!ses) return false;
    return !list_empty(ses->forward_history);
}

static void stop_tab_loading(int idx) {
    if (idx < 0 || idx >= s_tabs_cap || !s_tabs || !s_tabs[idx].used || !s_tabs[idx].dev) return;
    struct graphics_device *dev = s_tabs[idx].dev;
    if (dev->keyboard_handler) {
        dev->keyboard_handler(dev, KBD_CTRL_C, 0);
    }
    gui_set_str(&s_status_msg, "Stopped loading");
    s_dirty_chrome = true;
}

static void sync_default_font_size(bool force) {
    int base_px = gui_is_compact() ? 11 : (gui_is_medium() ? 13 : 16);
    int new_px = (base_px * s_zoom_pct) / 100;
    if (new_px < 8) new_px = 8;
    if (new_px > 32) new_px = 32;
    menu_font_size = ui_font_px();
    if (!force && dds.font_size == new_px) return;
    dds.font_size = new_px;

    for (int i = 0; i < s_tabs_cap; i++) {
        if (!s_tabs || !s_tabs[i].used || !s_tabs[i].dev) continue;
        struct session *ses = dev_to_session(s_tabs[i].dev);
        if (ses) {
            ses->ds.font_size = new_px;
            if (ses->screen) {
                html_interpret_recursive(ses->screen);
                if (i == s_active_tab && !s_tabs[i].is_home && !s_tabs[i].is_video) {
                    draw_formatted(ses);
                }
            }
        }
    }
}

static void apply_zoom_delta(int delta_pct) {
    if (delta_pct == 0) s_zoom_pct = 100;
    else s_zoom_pct += delta_pct;
    if (s_zoom_pct < 50)  s_zoom_pct = 50;
    if (s_zoom_pct > 200) s_zoom_pct = 200;

    sync_default_font_size(true);
    gui_set_fmt(&s_status_msg, "Page Zoom: %d%% (%dpx)", s_zoom_pct, dds.font_size);
    s_dirty_vp = true;
    s_dirty_chrome = true;
    if (s_overlay != OVL_NONE) s_dirty_overlay = true;
}

static void set_omnibox_str(const char *str) {
    if (!str) str = "";
    gui_dyn_set(&s_omnibox_text, &s_omnibox_cap, &s_omnibox_len, str);
    s_omnibox_cursor = 0;
    s_omnibox_scroll = 0;
    s_omnibox_sel_all = false;
    s_ac_sel = -1;
    rebuild_omnibox_autocomplete();
}

static void init_default_bookmarks(void) {
    static const struct {
        const char *label;
        const char *url;
        uint32_t col;
        char ch;
    } defs[] = {
        { "Home",      "about:home",                       COL_ACCENT_CYAN,  'H' },
        { "Google",    "https://www.google.com",           COL_ACCENT_BLUE,  'G' },
        { "YouTube",   "https://www.youtube.com",          COL_ACCENT_RED,   'Y' },
        { "Wikipedia", "https://en.wikipedia.org",         COL_TEXT_SUB,     'W' },
        { "Example",   "https://example.com",              COL_ACCENT_GREEN, 'E' },
        { "YT Player", "https://www.youtube.com/watch?v=YE7VzlLtp-4",         COL_ACCENT_PURP,  '>' }
    };
    for (int i = 0; i < s_bookmark_count; i++) {
        free(s_bookmarks[i].label);
        free(s_bookmarks[i].url);
        s_bookmarks[i].label = NULL;
        s_bookmarks[i].url = NULL;
    }
    s_bookmark_count = 0;
    for (int i = 0; i < (int)(sizeof(defs) / sizeof(defs[0])); i++) {
        if (!ensure_bookmark_cap(s_bookmark_count + 1)) break;
        ipo_bookmark_t *bm = &s_bookmarks[s_bookmark_count];
        bm->label = NULL;
        bm->url = NULL;
        if (!gui_set_str(&bm->label, defs[i].label) ||
            !gui_set_str(&bm->url, defs[i].url)) {
            free(bm->label);
            free(bm->url);
            bm->label = NULL;
            bm->url = NULL;
            break;
        }
        bm->badge_color = defs[i].col;
        bm->badge_char = defs[i].ch;
        bm->custom = false;
        s_bookmark_count++;
    }
}

static bool is_url_bookmarked(const char *url) {
    if (!url || !*url) return false;
    for (int i = 0; i < s_bookmark_count; i++) {
        if (s_bookmarks[i].url && strcmp(s_bookmarks[i].url, url) == 0) return true;
    }
    return false;
}

static void toggle_bookmark_current(void) {
    if (!s_tabs || s_active_tab < 0 || s_active_tab >= s_tabs_cap) return;
    ipo_tab_t *tab = &s_tabs[s_active_tab];
    const char *url = tab->is_home ? "about:home" : (tab->is_video ? gui_str(tab->video_url) : gui_str(tab->url));
    if (!url || !*url) return;
    for (int i = 0; i < s_bookmark_count; i++) {
        if (s_bookmarks[i].url && strcmp(s_bookmarks[i].url, url) == 0) {
            if (!s_bookmarks[i].custom) {
                gui_set_fmt(&s_status_msg, "Pinned bookmark: %s", gui_str(s_bookmarks[i].label));
                s_dirty_chrome = true;
                return;
            }
            free(s_bookmarks[i].label);
            free(s_bookmarks[i].url);
            for (int j = i; j + 1 < s_bookmark_count; j++) s_bookmarks[j] = s_bookmarks[j + 1];
            s_bookmark_count--;
            memset(&s_bookmarks[s_bookmark_count], 0, sizeof(s_bookmarks[s_bookmark_count]));
            gui_set_str(&s_status_msg, "Removed bookmark");
            s_dirty_chrome = true;
            return;
        }
    }
    if (!ensure_bookmark_cap(s_bookmark_count + 1)) {
        gui_set_str(&s_status_msg, "Out of memory");
        s_dirty_chrome = true;
        return;
    }
    ipo_bookmark_t *bm = &s_bookmarks[s_bookmark_count];
    bm->label = NULL;
    bm->url = NULL;
    const char *lbl = (tab->title && tab->title[0]) ? tab->title : url;
    if (!gui_set_str(&bm->label, lbl) || !gui_set_str(&bm->url, url)) {
        free(bm->label);
        free(bm->url);
        bm->label = NULL;
        bm->url = NULL;
        gui_set_str(&s_status_msg, "Out of memory");
        s_dirty_chrome = true;
        return;
    }
    bm->badge_color = COL_ACCENT_GOLD;
    bm->badge_char = (char)toupper((unsigned char)gui_str(bm->label)[0]);
    bm->custom = true;
    s_bookmark_count++;
    gui_set_fmt(&s_status_msg, "Bookmarked: %s", gui_str(bm->label));
    s_dirty_chrome = true;
}

static void sync_active_tab_meta(void) {
    if (!s_tabs || s_active_tab < 0 || s_active_tab >= s_tabs_cap) return;
    ipo_tab_t *tab = &s_tabs[s_active_tab];
    if (!tab->used || !tab->dev || tab->is_home || tab->is_video) return;
    struct session *ses = dev_to_session(tab->dev);
    if (!ses) return;
    if (!list_empty(ses->history)) {
        unsigned char *here = display_url(ses->term, cur_loc(ses)->url, 0);
        if (here && here[0]) {
            if (!tab->url || strcmp(tab->url, (const char *)here) != 0) {
                gui_set_str(&tab->url, (const char *)here);
                if (s_focus != FOCUS_OMNIBOX) {
                    set_omnibox_str(gui_str(tab->url));
                }
                record_history_visit(gui_str(tab->url), gui_str(tab->title));
                s_dirty_chrome = true;
            }
        }
        if (here) mem_free(here);
    }
    struct f_data_c *fd = current_frame(ses);
    if (fd && fd->f_data && fd->f_data->title && fd->f_data->title[0]) {
        const char *ctitle = (const char *)fd->f_data->title;
        if (!tab->title || strcmp(tab->title, ctitle) != 0) {
            gui_set_str(&tab->title, ctitle);
            record_history_visit(gui_str(tab->url), gui_str(tab->title));
            s_dirty_chrome = true;
        }
    }
}

static void trigger_tab_redraw(int idx) {
    if (!s_tabs || idx < 0 || idx >= s_tabs_cap || !s_tabs[idx].used) return;
    s_dirty_vp = true;
    s_dirty_chrome = true;
    if (s_tabs[idx].is_home || s_tabs[idx].is_video) {
        return;
    }
    struct graphics_device *dev = s_tabs[idx].dev;
    if (dev && dev->redraw_handler) {
        struct rect r = { 0, 0, ipo_gui_vp_w(), ipo_gui_vp_h() };
        dev->redraw_handler(dev, &r);
    }
}

static void enter_video_in_tab(int idx, const char *url) {
    if (!s_tabs || idx < 0 || idx >= s_tabs_cap || !s_tabs[idx].used) return;
    ipo_tab_t *tab = &s_tabs[idx];
    tab->is_home = false;
    tab->is_video = true;
    gui_set_str(&tab->video_url, (url && *url) ? url : "https://www.youtube.com/watch?v=YE7VzlLtp-4");
    char *vid = yt_extract_video_id_alloc(gui_str(tab->video_url));
    if (vid) {
        gui_set_fmt(&tab->video_title, "YouTube [%s]", vid);
        free(vid);
    } else {
        gui_set_str(&tab->video_title, "MP4 H.264 Stream Player");
    }
    gui_set_str(&tab->title, "YT: Play");
    record_history_visit(gui_str(tab->video_url), gui_str(tab->video_title));
    s_vid_paused = false;
    s_vid_pos_ms = 0;
    s_vid_last_tick_ms = gui_millis();
    if (idx == s_active_tab) {
        video_start_audio();
        s_focus = FOCUS_VIEWPORT;
        s_overlay = OVL_NONE;
        set_omnibox_str(gui_str(tab->video_url));
        gui_set_fmt(&s_status_msg, "Opening %s (Space Play/Pause, Left/Right Seek)", gui_str(tab->video_title));
    }
    s_dirty_vp = true;
    s_dirty_chrome = true;
}

static char *normalize_omnibox_query_alloc(const char *raw) {
    if (!raw) return gui_fmt_alloc("%s", "");
    while (*raw == ' ') raw++;
    size_t len = strlen(raw);
    while (len > 0 && raw[len - 1] == ' ') len--;
    if (len == 0) {
        return gui_fmt_alloc("about:home");
    }
    if ((len >= 7 && strncmp(raw, "http://", 7) == 0) ||
        (len >= 8 && strncmp(raw, "https://", 8) == 0) ||
        (len >= 7 && strncmp(raw, "file://", 7) == 0) ||
        (len >= 6 && strncmp(raw, "about:", 6) == 0) ||
        (len >= 5 && strncmp(raw, "data:", 5) == 0) ||
        (len >= 11 && strncasecmp(raw, "javascript:", 11) == 0) ||
        (len >= 8 && strncmp(raw, "links://", 8) == 0)) {
        return gui_fmt_alloc("%.*s", (int)len, raw);
    }
    bool has_space = false, has_dot = false;
    for (size_t i = 0; i < len; i++) {
        if (raw[i] == ' ') has_space = true;
        if (raw[i] == '.') has_dot = true;
    }
    if (!has_space && has_dot) {
        return gui_fmt_alloc("https://%.*s", (int)len, raw);
    }
    /* URL-encode query for Google Search */
    if (len > (SIZE_MAX - 1u) / 3u) return NULL;
    char *enc = (char *)malloc(len * 3u + 1u);
    if (!enc) return NULL;
    size_t ei = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            enc[ei++] = (char)c;
        } else if (c == ' ') {
            enc[ei++] = '+';
        } else {
            snprintf(enc + ei, 4, "%%%02X", c);
            ei += 3;
        }
    }
    enc[ei] = '\0';
    char *res = gui_fmt_alloc("https://www.google.com/search?q=%s", enc);
    free(enc);
    return res;
}

static void navigate_tab_to(int idx, const char *raw_input) {
    if (!s_tabs || idx < 0 || idx >= s_tabs_cap || !s_tabs[idx].used) return;
    if (!raw_input) raw_input = "";
    ipo_tab_t *tab = &s_tabs[idx];
    while (*raw_input == ' ') raw_input++;

    if (strncmp(raw_input, "/dl ", 4) == 0) {
        gui_start_download(raw_input + 4);
        return;
    }

    char *url = normalize_omnibox_query_alloc(raw_input);
    if (!url) {
        gui_set_str(&s_status_msg, "Out of memory");
        s_dirty_chrome = true;
        return;
    }

    if (strcmp(url, "about:home") == 0 || strcmp(url, "links://home") == 0) {
        free(url);
        if (tab->is_video) video_stop_audio();
        tab->is_home = true;
        tab->is_video = false;
        gui_set_str(&tab->url, "");
        gui_set_str(&tab->title, "New Tab");
        if (idx == s_active_tab) {
            s_overlay = OVL_NONE;
            set_omnibox_str("");
            gui_set_str(&s_status_msg, "Start Page");
        }
        s_dirty_vp = true;
        s_dirty_chrome = true;
        return;
    }

    if (strncasecmp(url, "javascript:", 11) == 0) {
        if (idx == s_active_tab) {
            s_focus = FOCUS_VIEWPORT;
            s_overlay = OVL_NONE;
            set_omnibox_str(tab->is_home ? "" : gui_str(tab->url));
            gui_set_str(&s_status_msg, "Executing JavaScript...");
            s_dirty_chrome = true;
        }
        struct session *ses = dev_to_session(tab->dev);
        if (ses) {
            goto_url_utf8(ses, (unsigned char *)url);
        }
        free(url);
        return;
    }

    if (yt_classify_url(url) == YT_URL_VIDEO) {
        enter_video_in_tab(idx, url);
        free(url);
        return;
    }

    if (tab->is_video) video_stop_audio();
    tab->is_home = false;
    tab->is_video = false;
    gui_set_str(&tab->url, url);

    /* Extract short host title while loading */
    const char *hp = strstr(url, "://");
    hp = hp ? hp + 3 : url;
    if (strncmp(hp, "www.", 4) == 0) hp += 4;
    const char *sl = strchr(hp, '/');
    size_t hl = sl ? (size_t)(sl - hp) : strlen(hp);
    if (hl > 0) gui_set_strn(&tab->title, hp, hl);
    else gui_set_str(&tab->title, "Loading...");

    record_history_visit(url, gui_str(tab->title));

    if (idx == s_active_tab) {
        s_focus = FOCUS_VIEWPORT;
        s_overlay = OVL_NONE;
        set_omnibox_str(url);
        gui_set_fmt(&s_status_msg, "Connecting to %s...", gui_str(tab->title));
    }

    /* Clear viewport cleanly before Links paints new page */
    fill_rect32(ipo_gui_vp_x(), ipo_gui_vp_y(), ipo_gui_vp_w(), ipo_gui_vp_h(), COL_WHITE);
    s_dirty_vp = true;
    s_dirty_chrome = true;
    ipo_gui_present_now();

    struct session *ses = dev_to_session(tab->dev);
    if (ses) {
        goto_url_utf8(ses, (unsigned char *)url);
    }
    free(url);
}

static void open_new_tab(const char *url) {
    if (ensure_tab_slot() < 0) {
        gui_set_str(&s_status_msg, "Out of memory for new tab");
        s_dirty_chrome = true;
        return;
    }
    gui_set_str(&s_pending_tab_url, (url && *url) ? url : "");
    int len = 0;
    void *info = create_session_info(-1, (unsigned char *)"", (unsigned char *)"", &len);
    unsigned char *cwd = get_cwd();
    if (!cwd) cwd = stracpy((unsigned char *)"");
    if (attach_g_terminal(cwd, info, len) < 0) {
        gui_set_str(&s_pending_tab_url, "");
    }
    mem_free(cwd);
}

static void close_tab(int idx) {
    if (!s_tabs || idx < 0 || idx >= s_tabs_cap || !s_tabs[idx].used) return;
    if (tab_count() <= 1) {
        if (s_tabs[idx].is_video) video_stop_audio();
        terminate_loop = 1;
        return;
    }
    if (s_tabs[idx].is_video && idx == s_active_tab) {
        video_stop_audio();
    }
    struct graphics_device *dev = s_tabs[idx].dev;
    struct terminal *term = dev_to_term(dev);
    if (term) {
        destroy_terminal(term);
    }
}

static void switch_to_tab(int idx) {
    if (!s_tabs || idx < 0 || idx >= s_tabs_cap || !s_tabs[idx].used) return;
    if (idx == s_active_tab) return;
    if (s_active_tab >= 0 && s_active_tab < s_tabs_cap && s_tabs[s_active_tab].is_video) {
        video_stop_audio();
    }
    s_active_tab = idx;
    ipo_tab_t *tab = &s_tabs[idx];
    s_focus = FOCUS_VIEWPORT;
    s_overlay = OVL_NONE;
    if (tab->is_home) {
        set_omnibox_str("");
        gui_set_str(&s_status_msg, "Start Page");
    } else if (tab->is_video) {
        set_omnibox_str(gui_str(tab->video_url));
        video_start_audio();
        s_vid_last_tick_ms = gui_millis();
    } else {
        set_omnibox_str(gui_str(tab->url));
    }
    trigger_tab_redraw(idx);
}

static void toggle_fullscreen(void) {
    s_fullscreen = !s_fullscreen;
    s_overlay = OVL_NONE;
    ipo_driver.x = ipo_gui_vp_w();
    ipo_driver.y = ipo_gui_vp_h();
    for (int i = 0; i < s_tabs_cap; i++) {
        if (!s_tabs[i].used || !s_tabs[i].dev) continue;
        struct graphics_device *dev = s_tabs[i].dev;
        dev->size.x1 = 0;
        dev->size.y1 = 0;
        dev->size.x2 = ipo_gui_vp_w();
        dev->size.y2 = ipo_gui_vp_h();
        set_clip_area(dev, &dev->size);
        if (dev->resize_handler) dev->resize_handler(dev);
    }
    trigger_tab_redraw(s_active_tab);
    gui_set_str(&s_status_msg, s_fullscreen ? "Fullscreen (Press F11 to exit)" : "Windowed Mode");
}

static void execute_find(bool backwards) {
    ipo_tab_t *tab = &s_tabs[s_active_tab];
    if (!tab->used || tab->is_home || tab->is_video || !s_find_text || !s_find_text[0]) return;
    struct session *ses = dev_to_session(tab->dev);
    if (!ses) return;
    struct f_data_c *f = current_frame(ses);
    if (!f) return;
    if (ses->search_word) mem_free(ses->search_word);
    ses->search_word = stracpy((unsigned char *)s_find_text);
    ses->search_direction = backwards ? -1 : 1;
    if (backwards) find_next_back(ses, f, 0);
    else find_next(ses, f, 0);
    s_dirty_vp = true;
    gui_set_fmt(&s_status_msg, "Search: \"%s\"", s_find_text);
}

/* =========================================================================
 * Modern Browser GUI Rendering
 * ========================================================================= */
typedef struct {
    int compact;
    int tight;
    int micro;
    int by0;
    int sub_y;
    int sx, sy, sw, sh, go_w;
    int gx0, gy0, cw, ch, gap_x, gap_y;
} start_layout_t;

typedef struct {
    int compact;
    int max_vis, vis_count, tw, f11_x, cls_x;
    int tx0, close_off, close_hit_off;
    int ctrl_w, ctrl_h, ctrl_y;
    int arrow_w, arrow_h, arrow_step, arrow_y;
    int tab_y, tab_h, tab_gap;
    int plus_w, plus_h, plus_y;
} tabbar_layout_t;

typedef struct {
    int compact;
    int show_extra;
    int nav_x0, nav_w, nav_step;
    int ox, oy, ow, oh, badge_w, star_w;
    int dl_x, hist_x, find_x, vid_x, menu_x;
    int act_w, vid_w;
} toolbar_layout_t;

typedef struct {
    int compact;
    int sby, sbh, zx, btn_w, pct_x, pct_w, plus_x, show_hint;
} statusbar_layout_t;

typedef struct {
    int compact;
    int pad, hdr_h, ctrl_h;
    int back_x, back_y, back_w, back_h;
    int vid_x, vid_y, vid_w, vid_h;
    int ctrl_y, sb_x, sb_y, sb_w, btn_y, btn_h;
    int play_x, play_w, b5_x, b5_w, f5_x, f5_w, mute_x, mute_w, full_x, full_w;
} video_layout_t;

static void compute_start_layout(int vx, int vy, int vw, int vh, start_layout_t *L) {
    L->compact = (vh < 390) ? 1 : 0;
    L->tight   = (vh < 260) ? 1 : 0;
    L->micro   = (vw < 540 || vh < 220) ? 1 : 0;

    if (L->micro) {
        int show_sub = (vh >= 112);
        L->by0   = vy + 2;
        L->sub_y = vy + 11;
        L->sw    = vw - 14;
        if (L->sw > 440) L->sw = 440;
        if (L->sw < 160) L->sw = (vw > 8) ? (vw - 8) : vw;
        L->sx    = vx + (vw - L->sw) / 2;
        L->sh    = 14;
        L->go_w  = 32;
        L->sy    = show_sub ? (vy + 21) : (vy + 12);

        L->gap_x = 4;
        L->gap_y = 3;
        L->cw    = (vw - 14 - 2 * L->gap_x) / 3;
        if (L->cw < 60) L->cw = 60;
        if (L->cw > 160) L->cw = 160;
        L->gx0   = vx + (vw - (3 * L->cw + 2 * L->gap_x)) / 2;
        L->gy0   = L->sy + L->sh + 4;

        int avail_h = vh - (L->gy0 - vy) - 3;
        L->ch = (avail_h - L->gap_y) / 2;
        if (L->ch < 16) L->ch = 16;
        if (L->ch > 46) L->ch = 46;
        return;
    }

    L->by0     = L->tight ? (vy + 8) : (L->compact ? (vy + 14) : (vy + 58));
    L->sub_y   = L->tight ? (L->by0 + 32) : (L->by0 + 38);

    L->sw = vw - (vw >= 760 ? 200 : 120);
    if (L->sw > 640) L->sw = 640;
    if (L->sw < 360) L->sw = vw - 40;
    L->sx = vx + (vw - L->sw) / 2;
    L->sh = L->compact ? 32 : 40;
    L->go_w = 66;
    L->sy = L->tight ? (vy + 60) : (L->compact ? (vy + 72) : (vy + 132));

    L->gap_x = (vw >= 760) ? 17 : 12;
    L->gap_y = L->tight ? 8 : (L->compact ? 10 : 16);
    int pad_x = (vw >= 760) ? 68 : 24;
    L->cw = (vw - 2 * pad_x - 2 * L->gap_x) / 3;
    if (L->cw > 260) L->cw = 260;
    if (L->cw < 150) L->cw = 150;
    L->gx0 = vx + (vw - (3 * L->cw + 2 * L->gap_x)) / 2;
    L->gy0 = L->sy + L->sh + (L->tight ? 10 : (L->compact ? 14 : 24));

    int avail_h = vh - (L->gy0 - vy) - (L->tight ? 8 : (L->compact ? 10 : 16));
    L->ch = (avail_h - L->gap_y) / 2;
    if (L->ch > 116) L->ch = 116;
    if (L->ch < 48)  L->ch = 48;
}

static void compute_hero_text_rect(const start_layout_t *L, int *out_x, int *out_w, int *out_y) {
    int ch = ui_char_h();
    if (L->micro) {
        *out_x = L->sx + 16;
        *out_w = L->sw - L->go_w - 22;
    } else {
        *out_x = L->sx + 38;
        *out_w = L->sw - 124;
    }
    if (*out_w < 16) *out_w = 16;
    *out_y = L->sy + (L->sh - ch) / 2;
}

static void compute_tabbar_layout_ex(int n_tabs, tabbar_layout_t *L) {
    int w = s_gui_w;
    bool compact = gui_is_compact();
    L->compact = compact ? 1 : 0;
    if (n_tabs < 1) n_tabs = 1;

    if (compact) {
        L->tx0 = 3;
        L->close_off = 8;
        L->close_hit_off = 13;
        L->ctrl_w = 13; L->ctrl_h = 11; L->ctrl_y = 1;
        L->cls_x = w - 15;
        L->f11_x = w - 30;
        L->arrow_w = 11; L->arrow_h = 11; L->arrow_step = 12; L->arrow_y = 1;
        L->tab_y = 1; L->tab_h = IPO_TABBAR_H - 1; L->tab_gap = 2;
        L->plus_w = 13; L->plus_h = 11; L->plus_y = 1;

        int max_vis = (w - 60) / 68;
        if (max_vis < 2) max_vis = 2;
        if (max_vis > 5) max_vis = 5;
        int vis_count = (n_tabs < max_vis) ? n_tabs : max_vis;
        bool overflow = (n_tabs > max_vis);
        int avail_w = L->f11_x - 4 - L->tx0 - (overflow ? 2 * L->arrow_step : 0) - (L->plus_w + 3);
        int tw = avail_w / vis_count;
        if (tw > 140) tw = 140;
        if (tw < 54) tw = 54;
        L->max_vis = max_vis;
        L->vis_count = vis_count;
        L->tw = tw;
        return;
    }

    L->tx0 = 6;
    L->close_off = 15;
    L->close_hit_off = 25;
    L->ctrl_h = IPO_TABBAR_H - 8;
    if (L->ctrl_h < 16) L->ctrl_h = 16;
    if (L->ctrl_h > 22) L->ctrl_h = 22;
    L->ctrl_w = L->ctrl_h + 6;
    L->ctrl_y = (IPO_TABBAR_H - L->ctrl_h) / 2;
    L->cls_x = w - L->ctrl_w - 6;
    L->f11_x = L->cls_x - L->ctrl_w - 4;
    L->arrow_w = 18; L->arrow_h = L->ctrl_h; L->arrow_step = 20; L->arrow_y = L->ctrl_y;
    L->tab_y = 3; L->tab_h = IPO_TABBAR_H - 3; L->tab_gap = 3;
    L->plus_w = L->ctrl_h; L->plus_h = L->ctrl_h; L->plus_y = L->ctrl_y;

    int max_vis = (w - 150) / 96;
    if (max_vis < 3) max_vis = 3;
    if (max_vis > 8) max_vis = 8;
    int vis_count = n_tabs < max_vis ? n_tabs : max_vis;
    bool overflow = (n_tabs > max_vis);
    int avail_w = w - (overflow ? 160 : 112);
    int tw = avail_w / vis_count;
    if (tw > 180) tw = 180;
    int min_tw = (w >= 720) ? 92 : 76;
    if (tw < min_tw) tw = min_tw;
    L->max_vis = max_vis;
    L->vis_count = vis_count;
    L->tw = tw;
}

static void compute_tabbar_layout(int n_tabs, int *out_max_vis, int *out_vis_count,
                                  int *out_tw, int *out_f11_x, int *out_cls_x) {
    tabbar_layout_t L;
    compute_tabbar_layout_ex(n_tabs, &L);
    *out_max_vis   = L.max_vis;
    *out_vis_count = L.vis_count;
    *out_tw        = L.tw;
    *out_f11_x     = L.f11_x;
    *out_cls_x     = L.cls_x;
}

static void compute_toolbar_layout_ex(toolbar_layout_t *L) {
    int w = s_gui_w;
    bool compact = gui_is_compact();
    L->compact = compact ? 1 : 0;
    L->show_extra = (!compact || w >= 440) ? 1 : 0;
    if (compact) {
        L->nav_x0 = 3; L->nav_w = 12; L->nav_step = 13;
        L->oy = IPO_TABBAR_H + 1;
        L->oh = IPO_TOOLBAR_H - 2;
        if (L->show_extra) {
            L->act_w = 14; L->vid_w = 16;
            L->menu_x = w - 16;
            L->vid_x  = w - 34;
            L->find_x = w - 50;
            L->hist_x = w - 66;
            L->dl_x   = w - 82;
            L->ox = 57;
            L->ow = (L->dl_x - 3) - L->ox;
        } else {
            L->act_w = 13; L->vid_w = 15;
            L->menu_x = w - 15;
            L->vid_x  = w - 32;
            L->find_x = -100;
            L->hist_x = -100;
            L->dl_x   = -100;
            L->ox = 57;
            L->ow = (L->vid_x - 3) - L->ox;
        }
        if (L->ow < 80) L->ow = 80;
        L->badge_w = 12;
        L->star_w = 12;
        return;
    }

    L->nav_x0 = 6; L->nav_w = 24; L->nav_step = 26;
    L->act_w = 26; L->vid_w = 32;
    L->menu_x = w - 32;
    L->vid_x  = w - 66;
    L->find_x = w - 94;
    L->hist_x = w - 122;
    L->dl_x   = w - 150;
    L->ox = 114;
    L->oy = IPO_TABBAR_H + 3;
    L->ow = (L->dl_x - 5) - L->ox;
    if (L->ow < 80) L->ow = 80;
    L->oh = IPO_TOOLBAR_H - 6;
    L->badge_w = 20;
    L->star_w = 20;
}

static void compute_omnibox_text_rect(const toolbar_layout_t *BL, int *out_x, int *out_w, int *out_y) {
    *out_x = BL->ox + 3 + BL->badge_w + (BL->compact ? 3 : 4);
    *out_w = BL->ow - (BL->badge_w + BL->star_w + (BL->compact ? 11 : 14));
    if (*out_w < 16) *out_w = 16;
    *out_y = BL->oy + (BL->oh - ui_char_h()) / 2;
}

static void compute_toolbar_layout(int *ox, int *oy, int *ow, int *oh,
                                   int *dl_x, int *hist_x, int *find_x, int *vid_x, int *menu_x) {
    toolbar_layout_t L;
    compute_toolbar_layout_ex(&L);
    *ox = L.ox; *oy = L.oy; *ow = L.ow; *oh = L.oh;
    *dl_x = L.dl_x; *hist_x = L.hist_x; *find_x = L.find_x;
    *vid_x = L.vid_x; *menu_x = L.menu_x;
}

static void compute_statusbar_layout(statusbar_layout_t *L) {
    int w = s_gui_w;
    int h = s_gui_h;
    L->compact = gui_is_compact() ? 1 : 0;
    L->sbh = IPO_STATUSBAR_H;
    L->sby = h - L->sbh;
    if (L->compact) {
        L->zx = w - 66;
        L->btn_w = 14;
        L->pct_x = L->zx + 16;
        L->pct_w = 32;
        L->plus_x = L->zx + 50;
        L->show_hint = 0;
    } else {
        L->zx = w - 340;
        L->btn_w = 18;
        L->pct_x = L->zx + 20;
        L->pct_w = 46;
        L->plus_x = L->zx + 68;
        L->show_hint = 1;
    }
}

static void compute_video_layout(int vx, int vy, int vw, int vh, video_layout_t *V) {
    V->compact = (vw < 540 || vh < 220) ? 1 : 0;
    if (V->compact) {
        V->pad = 4;
        V->hdr_h = 16;
        V->ctrl_h = 22;
        V->back_w = 48;
        V->back_h = 12;
        V->back_x = vx + vw - V->pad - V->back_w;
        V->back_y = vy + 2;
        V->vid_x = vx + V->pad;
        V->vid_y = vy + V->hdr_h;
        V->vid_w = vw - 2 * V->pad;
        V->vid_h = vh - V->hdr_h - V->ctrl_h - 3;
        if (V->vid_h < 32) V->vid_h = 32;
        V->ctrl_y = V->vid_y + V->vid_h + 2;
        V->sb_x = V->vid_x + 5;
        V->sb_y = V->ctrl_y + 2;
        V->sb_w = V->vid_w - 10;
        V->btn_y = V->ctrl_y + 7;
        V->btn_h = 13;
        V->play_x = V->vid_x + 3;   V->play_w = 40;
        V->b5_x   = V->vid_x + 46;  V->b5_w   = 23;
        V->f5_x   = V->vid_x + 72;  V->f5_w   = 23;
        V->mute_x = V->vid_x + 98;  V->mute_w = 32;
        V->full_w = 34;
        V->full_x = V->vid_x + V->vid_w - V->full_w - 3;
        return;
    }

    V->pad = 20;
    V->hdr_h = 52;
    V->ctrl_h = 60;
    V->back_w = 110;
    V->back_h = 24;
    V->back_x = vx + vw - V->pad - 118;
    V->back_y = vy + 15;
    V->vid_x = vx + V->pad;
    V->vid_y = vy + V->hdr_h;
    V->vid_w = vw - 2 * V->pad;
    V->vid_h = vh - 126;
    if (V->vid_h < (vh < 220 ? 72 : 96)) V->vid_h = (vh < 220 ? 72 : 96);
    V->ctrl_y = V->vid_y + V->vid_h + 8;
    V->sb_x = V->vid_x + 14;
    V->sb_y = V->ctrl_y + 12;
    V->sb_w = V->vid_w - 28;
    V->btn_y = V->ctrl_y + 26;
    V->btn_h = 26;
    V->play_x = V->vid_x + 14;  V->play_w = 92;
    V->b5_x   = V->vid_x + 114; V->b5_w   = 54;
    V->f5_x   = V->vid_x + 174; V->f5_w   = 54;
    V->mute_x = V->vid_x + 234; V->mute_w = 72;
    V->full_w = 96;
    V->full_x = V->vid_x + V->vid_w - 110;
}

static bool menu_is_2col(void) {
    int top_start = s_fullscreen ? 2 : (IPO_TOP_CHROME + 2);
    int bottom_lim = s_fullscreen ? (s_gui_h - 2) : (s_gui_h - IPO_STATUSBAR_H - 2);
    return (s_gui_w >= 260) && ((bottom_lim - top_start) < 220);
}

static void compute_overlay_rect(overlay_kind_t kind, int *rx, int *ry, int *rw, int *rh, int *out_param) {
    int w = s_gui_w;
    int h = s_gui_h;
    int top_start = s_fullscreen ? 2 : (IPO_TOP_CHROME + 2);
    int bottom_lim = s_fullscreen ? (h - 2) : (h - IPO_STATUSBAR_H - 2);
    int avail = bottom_lim - top_start;
    if (avail < 40) avail = 40;
    *out_param = 0;

    if (kind == OVL_FIND) {
        *rw = (w >= 336) ? 310 : (w - 8);
        *rh = ui_is_compact_font() ? 18 : 34;
        *rx = w - *rw - 4;
        *ry = top_start;
    } else if (kind == OVL_SECURITY) {
        *rw = (w >= 376) ? 360 : (w - 8);
        int want_h = ui_is_compact_font() ? 98 : 178;
        *rh = (avail < want_h) ? avail : want_h;
        *rx = (w >= 520) ? 140 : ((w - *rw) / 2);
        if (*rx + *rw > w - 4) *rx = w - *rw - 4;
        if (*rx < 4) *rx = 4;
        *ry = top_start;
    } else if (kind == OVL_MENU) {
        *ry = top_start;
        if (menu_is_2col()) {
            int top_h = ui_is_compact_font() ? 18 : 28;
            *rw = (w >= 304) ? 296 : (w - 8);
            *rx = w - *rw - 4;
            int step = (avail - top_h) / 5;
            int max_step = ui_is_compact_font() ? 16 : 20;
            int min_step = ui_is_compact_font() ? 11 : 14;
            if (step > max_step) step = max_step;
            if (step < min_step) step = min_step;
            *rh = top_h + step * 5;
            if (*rh > avail) *rh = avail;
            *out_param = step;
        } else {
            *rw = (w >= 250) ? 234 : (w - 8);
            *rx = w - *rw - 4;
            int step = (avail >= 286) ? 26 : ((avail - 34) / 9);
            if (step < 14) step = 14;
            *rh = 34 + step * 9;
            if (*rh > avail) *rh = avail;
            *out_param = step;
        }
    } else if (kind == OVL_HISTORY) {
        int hdr = ui_is_compact_font() ? 22 : 42;
        int row_h = ui_is_compact_font() ? 22 : 36;
        *rw = (w >= 428) ? 412 : (w - 8);
        *rx = w - *rw - 4;
        int max_items = (avail - hdr) / row_h;
        if (max_items < 1) max_items = 1;
        *ry = top_start;
        *rh = hdr + max_items * row_h;
        if (*rh > avail) *rh = avail;
        *out_param = max_items;
    } else if (kind == OVL_DOWNLOADS) {
        *rw = (w >= 398) ? 382 : (w - 8);
        *rx = w - *rw - 4;
        int hdr_h = ui_is_compact_font() ? 18 : ((avail < 140) ? 28 : 36);
        int btn_area = ui_is_compact_font() ? 20 : ((avail < 140) ? 30 : 42);
        int row_h = ui_is_compact_font() ? 26 : 48;
        int max_slots = (avail - hdr_h - btn_area - 4) / row_h;
        if (max_slots < 0) max_slots = 0;
        *ry = top_start;
        *rh = hdr_h + 4 + (max_slots > 0 ? max_slots : 1) * row_h + btn_area;
        if (*rh > avail) *rh = avail;
        *out_param = max_slots;
    } else {
        *rx = 0; *ry = 0; *rw = 0; *rh = 0;
    }
}

static void compute_autocomplete_layout(int *ax, int *ay, int *aw, int *ah, int *row_h, int *vis_count) {
    toolbar_layout_t T;
    compute_toolbar_layout_ex(&T);
    int w = s_gui_w;
    int h = s_gui_h;
    int x = T.ox;
    int width = T.ow;
    if (width < 200) {
        width = (w >= 240) ? (w - 12) : (w - 8);
        x = (w - width) / 2;
    }
    int y = s_fullscreen ? 24 : (IPO_TABBAR_H + IPO_TOOLBAR_H - 1);
    int rh = ui_is_compact_font() ? 13 : 24;
    int bottom_lim = h - (s_fullscreen ? 4 : IPO_STATUSBAR_H) - 4;
    int max_rows = (bottom_lim - y - 6) / rh;
    if (max_rows < 1) max_rows = 1;
    int vis = (s_ac_count < max_rows) ? s_ac_count : max_rows;
    *ax = x;
    *ay = y;
    *aw = width;
    *row_h = rh;
    *vis_count = vis;
    *ah = 6 + vis * rh;
}

static void compute_find_layout(int fx, int fy, int fw, int fh,
                                int *in_x, int *in_w, int *prv_x, int *nxt_x, int *cls_x,
                                int *nav_w, int *cls_w, int *btn_y, int *btn_h) {
    bool compact_f = (fh <= 22);
    *btn_h = compact_f ? (fh - 4) : 24;
    *btn_y = fy + (fh - *btn_h) / 2;
    *cls_w = *btn_h;
    *nav_w = compact_f ? 16 : 32;
    *cls_x = fx + fw - 4 - *cls_w;
    *nxt_x = *cls_x - 3 - *nav_w;
    *prv_x = *nxt_x - 3 - *nav_w;
    *in_x  = fx + 4;
    *in_w  = *prv_x - 3 - *in_x;
}

static void compute_context_layout(int *cx, int *cy, int *cw, int *ch,
                                   int *row_h, int *pad_y, int *n_items) {
    int n = s_ctx_has_link ? 10 : 7;
    int w = s_gui_w;
    int h = s_gui_h;
    int width = (w >= 244) ? 228 : (w - 8);
    int min_y = s_fullscreen ? 2 : (IPO_TOP_CHROME + 2);
    int max_y = h - (s_fullscreen ? 2 : IPO_STATUSBAR_H) - 2;
    if ((max_y - min_y) < n * 20 + 6 && !s_fullscreen) min_y = IPO_TABBAR_H + 2;
    if ((max_y - min_y) < n * 16 + 6) {
        min_y = 2;
        max_y = h - 2;
    }
    int avail = max_y - min_y;
    int rh = (avail >= n * 25 + 10) ? 25 : ((avail - 6) / n);
    if (rh < 14) rh = 14;
    int py = (rh >= 20) ? 5 : 3;
    int height = py * 2 + n * rh;
    int x = s_ctx_x;
    int y = s_ctx_y;
    if (x + width > w - 4) x = w - width - 4;
    if (y + height > max_y) y = max_y - height;
    if (x < 4) x = 4;
    if (y < min_y) y = min_y;
    *n_items = n;
    *cw = width;
    *ch = height;
    *cx = x;
    *cy = y;
    *row_h = rh;
    *pad_y = py;
}

static void compute_downloads_btn_layout(int dx, int dy, int dw, int dh,
                                         int *by, int *btn_h, int *bx0, int *bw, int *bstep) {
    *btn_h = ui_is_compact_font() ? 14 : ((dh < 130) ? 22 : 30);
    *by = dy + dh - (*btn_h + (ui_is_compact_font() ? 4 : 8));
    *bw = (dw - (ui_is_compact_font() ? 24 : 36)) / 3;
    *bx0 = dx + (ui_is_compact_font() ? 8 : 10);
    *bstep = *bw + (ui_is_compact_font() ? 4 : 8);
}

typedef struct {
    int mx, my, mw, mh, row_step;
    bool two_col, compact;
    int z_y, z_h, sep_y, row_y0;
    int minus_x, minus_w, pct_x, pct_w, plus_x, plus_w;
    int col_w;
} menu_layout_t;

static void compute_menu_layout(menu_layout_t *M) {
    compute_overlay_rect(OVL_MENU, &M->mx, &M->my, &M->mw, &M->mh, &M->row_step);
    M->two_col = menu_is_2col();
    M->compact = ui_is_compact_font() != 0;
    if (M->compact) {
        M->z_y = M->my + 2;
        M->z_h = 13;
        M->sep_y = M->my + 16;
        M->row_y0 = M->two_col ? (M->my + 18) : (M->my + 20);
        M->plus_w = 18; M->pct_w = 38; M->minus_w = 18;
        M->plus_x  = M->mx + M->mw - 6 - M->plus_w;
        M->pct_x   = M->plus_x - 3 - M->pct_w;
        M->minus_x = M->pct_x - 3 - M->minus_w;
    } else {
        M->z_y = M->two_col ? (M->my + 4) : (M->my + 6);
        M->z_h = M->two_col ? 19 : 22;
        M->sep_y = M->two_col ? (M->my + 26) : (M->my + 33);
        M->row_y0 = M->two_col ? (M->my + 28) : (M->my + 38);
        M->plus_w = 28; M->pct_w = 56; M->minus_w = 28;
        M->minus_x = M->mx + M->mw - 142;
        M->pct_x   = M->mx + M->mw - 110;
        M->plus_x  = M->mx + M->mw - 50;
    }
    M->col_w = (M->mw - 18) / 2;
}

static void draw_start_page(void) {
    int vx = ipo_gui_vp_x();
    int vy = ipo_gui_vp_y();
    int vw = ipo_gui_vp_w();
    int vh = ipo_gui_vp_h();

    start_layout_t L;
    compute_start_layout(vx, vy, vw, vh, &L);

    /* Crisp dark slate background (solid in compact mode, clean banded gradient in desktop) */
    for (int ry = 0; ry < vh; ry++) {
        uint32_t row_col = L.micro ? 0x0B1120u : lerp_color(0x0B1120u, 0x162033u, (ry * 256) / vh);
        uint32_t *dst = s_fb32 + (vy + ry) * IPO_GUI_W + vx;
        for (int rx = 0; rx < vw; rx++) dst[rx] = row_col;
    }

    const char *brand = "LINKS";
    static const uint32_t bcols[5] = {
        0x3B82F6u, 0x38BDF8u, 0xA855F7u, 0x10B981u, 0xF59E0Bu
    };
    static const struct {
        const char *title;
        const char *short_title;
        const char *desc;
        const char *tag;
        uint32_t col;
        char glyph;
    } cards[6] = {
        { "Google Search", "Google",  "HTTPS Web Search Engine",   "google.com",    COL_ACCENT_BLUE,  'G' },
        { "YouTube",       "YouTube", "HTTPS Browse & Stream",     "youtube.com",   COL_ACCENT_RED,   'Y' },
        { "YT HD Player",  "YT Play", "Video + SB16 Stereo Audio", "1080p Player",  COL_ACCENT_PURP,  '>' },
        { "Wikipedia",     "Wiki",    "Free Encyclopedia (HTTPS)", "wikipedia.org", COL_TEXT_SUB,     'W' },
        { "Example Site",  "Example", "Fast TLS Handshake Test",   "example.com",   COL_ACCENT_GREEN, 'E' },
        { "Downloads",     "Files",   "Save Files to /data/...",   "Ctrl+J",        0xF59E0Bu,        'D' }
    };

    if (L.micro) {
        int ch_f = ui_char_h();
        int links_w = ipo_font_text_width(IPO_FONT_BOLD, 12, "LINKS ");
        int brows_w = ipo_font_text_width(IPO_FONT_BOLD, 12, "BROWSER");
        int bx0 = vx + (vw - (links_w + brows_w)) / 2;
        int cur_x = bx0;
        for (int i = 0; i < 5; i++) {
            cur_x += ipo_font_draw_glyph_rgb32(s_fb32, s_gui_w, 0, 0, s_gui_w, s_gui_h,
                                               cur_x, L.by0, IPO_FONT_BOLD, 12,
                                               (uint32_t)brand[i], bcols[i]);
        }
        cur_x += ipo_font_measure_glyph(IPO_FONT_BOLD, 12, ' ');
        draw_text_styled32(cur_x, L.by0, "BROWSER", COL_TEXT_MAIN, brows_w + 8, IPO_FONT_BOLD, 12);

        if (vh >= 112) {
            const char *sub = "HTTPS  |  Downloads  |  YouTube";
            int sub_w = ui_text_w(sub);
            int sub_x = vx + (vw - sub_w) / 2;
            if (sub_x < vx + 4) sub_x = vx + 4;
            draw_text_ellipsis32(sub_x, L.sub_y, sub, COL_TEXT_SUB, vw - 8);
        }

        int sx = L.sx, sy = L.sy, sw = L.sw, sh = L.sh;
        fill_rrect32(sx, sy, sw, sh, 4, COL_OMNIBOX_BG);
        stroke_rrect32(sx, sy, sw, sh, 4, (s_focus == FOCUS_HERO) ? COL_ACCENT_BLUE : COL_CARD_BD);
        fill_circle32(sx + 8, sy + sh / 2 - 1, 2, COL_TEXT_SUB);
        draw_line32(sx + 10, sy + sh / 2 + 1, sx + 12, sy + sh / 2 + 3, COL_TEXT_SUB);

        int hx, hw, hy;
        compute_hero_text_rect(&L, &hx, &hw, &hy);
        draw_text_scrolled32(hx, hy, s_hero_text, s_hero_len, s_hero_cursor, &s_hero_scroll,
                             s_focus == FOCUS_HERO, s_hero_sel_all,
                             COL_TEXT_MAIN, COL_TEXT_DIM, "Search or URL...",
                             hw, sy + 1, sh - 2);
        fill_rrect32(sx + sw - L.go_w - 4, sy + 2, L.go_w, sh - 4, 2, (s_hover_id == 900) ? 0x3B82F6u : COL_ACCENT_BLUE);
        {
            int ow_txt = ui_text_w("Open");
            int ox_txt = (sx + sw - L.go_w - 4) + (L.go_w - ow_txt) / 2;
            draw_text_clip32(ox_txt, hy, "Open", COL_WHITE, L.go_w - 4, 1);
        }

        int gx0 = L.gx0, gy0 = L.gy0, cw_c = L.cw, ch_c = L.ch, gap_x = L.gap_x, gap_y = L.gap_y;
        for (int i = 0; i < 6; i++) {
            int cx = gx0 + (i % 3) * (cw_c + gap_x);
            int cy = gy0 + (i / 3) * (ch_c + gap_y);
            bool hov = (s_hover_id == 910 + i);
            fill_rrect32(cx, cy, cw_c, ch_c, 4, hov ? COL_CARD_HOVER : COL_CARD_BG);
            stroke_rrect32(cx, cy, cw_c, ch_c, 4, hov ? COL_ACCENT_BLUE : COL_CARD_BD);
            int bx_c = cx + 9;
            int by_c = cy + ch_c / 2;
            fill_circle32(bx_c, by_c, 5, cards[i].col);
            if (cards[i].glyph == '>') {
                draw_icon_play(bx_c + 1, by_c, 2, COL_WHITE);
            } else {
                int gw_c = ui_glyph_adv((uint32_t)cards[i].glyph);
                draw_glyph32(bx_c - gw_c / 2, by_c - ch_f / 2, (uint32_t)cards[i].glyph, COL_WHITE, 1);
            }
            draw_text_ellipsis32(cx + 17, cy + (ch_c - ch_f) / 2, cards[i].short_title, COL_TEXT_MAIN, cw_c - 19);
        }
        return;
    }

    /* Desktop / Medium Crisp Start Page */
    int brand_px = L.compact ? 22 : 28;
    int links_w = ipo_font_text_width(IPO_FONT_BOLD, brand_px, "LINKS ");
    int brows_w = ipo_font_text_width(IPO_FONT_BOLD, brand_px, "BROWSER");
    int bx0 = vx + (vw - (links_w + brows_w)) / 2;
    int by0 = L.by0;

    if (!L.compact) {
        const char *pill = "TLS 1.2 + TrueColor";
        int pw = ui_text_w(pill) + 32;
        fill_rrect32(vx + 36, vy + 16, pw, 24, 6, COL_CARD_BG);
        stroke_rrect32(vx + 36, vy + 16, pw, 24, 6, COL_CARD_BD);
        fill_circle32(vx + 49, vy + 28, 3, COL_ACCENT_GREEN);
        draw_text_clip32(vx + 59, vy + 28 - ui_char_h() / 2, pill, COL_TEXT_SUB, pw - 26, 1);
    } else if (bx0 >= vx + 170) {
        const char *pill = "TLS 1.2 + TrueColor";
        int pw = ui_text_w(pill) + 28;
        fill_rrect32(vx + 12, vy + 8, pw, 22, 6, COL_CARD_BG);
        stroke_rrect32(vx + 12, vy + 8, pw, 22, 6, COL_CARD_BD);
        fill_circle32(vx + 23, vy + 19, 3, COL_ACCENT_GREEN);
        draw_text_clip32(vx + 32, vy + 19 - ui_char_h() / 2, pill, COL_TEXT_SUB, pw - 24, 1);
    }

    {
        int cur_x = bx0;
        for (int i = 0; i < 5; i++) {
            ipo_font_draw_glyph_rgb32(s_fb32, s_gui_w, 0, 0, s_gui_w, s_gui_h,
                                      cur_x + 2, by0 + 2, IPO_FONT_BOLD, brand_px,
                                      (uint32_t)brand[i], 0x050811u);
            cur_x += ipo_font_draw_glyph_rgb32(s_fb32, s_gui_w, 0, 0, s_gui_w, s_gui_h,
                                               cur_x, by0, IPO_FONT_BOLD, brand_px,
                                               (uint32_t)brand[i], bcols[i]);
        }
        cur_x += ipo_font_measure_glyph(IPO_FONT_BOLD, brand_px, ' ');
        draw_text_styled32(cur_x + 2, by0 + 2, "BROWSER", 0x050811u, brows_w + 16, IPO_FONT_BOLD, brand_px);
        draw_text_styled32(cur_x,     by0,     "BROWSER", COL_TEXT_MAIN, brows_w + 16, IPO_FONT_BOLD, brand_px);
    }

    const char *sub = "IPO_OS Modern Desktop Browser  -  HTTP / HTTPS  -  Downloads  -  YouTube";
    int sub_w = ui_text_w(sub);
    int sub_x = vx + (vw - sub_w) / 2;
    if (sub_x < vx + 16) sub_x = vx + 16;
    draw_text_ellipsis32(sub_x, L.sub_y, sub, COL_TEXT_SUB, vw - 32);

    int sx = L.sx, sy = L.sy, sw = L.sw, sh = L.sh;
    fill_rrect32(sx, sy, sw, sh, 10, COL_OMNIBOX_BG);
    stroke_rrect32(sx, sy, sw, sh, 10, (s_focus == FOCUS_HERO) ? COL_ACCENT_BLUE : COL_CARD_BD);
    if (s_focus == FOCUS_HERO) {
        stroke_rrect32(sx + 1, sy + 1, sw - 2, sh - 2, 9, COL_ACCENT_BLUE);
    }

    int lens_y = sy + sh / 2 - 2;
    fill_circle32(sx + 20, lens_y, 5, COL_TEXT_SUB);
    fill_circle32(sx + 20, lens_y, 3, COL_OMNIBOX_BG);
    draw_line32(sx + 24, lens_y + 4, sx + 28, lens_y + 8, COL_TEXT_SUB);

    int hx, hw, htext_y;
    compute_hero_text_rect(&L, &hx, &hw, &htext_y);
    draw_text_scrolled32(hx, htext_y, s_hero_text, s_hero_len, s_hero_cursor, &s_hero_scroll,
                         s_focus == FOCUS_HERO, s_hero_sel_all,
                         COL_TEXT_MAIN, COL_TEXT_DIM,
                         "Search Google or type a URL (google.com, youtube.com)...",
                         hw, sy + 5, sh - 10);

    fill_rrect32(sx + sw - 74, sy + 4, L.go_w, sh - 8, 7, (s_hover_id == 900) ? 0x3B82F6u : COL_ACCENT_BLUE);
    {
        int go_tw = ui_text_w("Open");
        int go_tx = (sx + sw - 74) + (L.go_w - go_tw) / 2;
        draw_text_styled32(go_tx, htext_y, "Open", COL_WHITE, L.go_w - 8, IPO_FONT_BOLD, ui_font_px());
    }

    int gx0 = L.gx0, gy0 = L.gy0, cw = L.cw, ch = L.ch, gap_x = L.gap_x, gap_y = L.gap_y;
    for (int i = 0; i < 6; i++) {
        int col_i = i % 3;
        int row_i = i / 3;
        int cx = gx0 + col_i * (cw + gap_x);
        int cy = gy0 + row_i * (ch + gap_y);
        bool hov = (s_hover_id == 910 + i);
        fill_rrect32(cx, cy, cw, ch, 10, hov ? COL_CARD_HOVER : COL_CARD_BG);
        stroke_rrect32(cx, cy, cw, ch, 10, hov ? COL_ACCENT_BLUE : COL_CARD_BD);

        int by_c = (ch >= 88) ? (cy + 28) : (cy + 23);
        int bx_c = (ch >= 88) ? (cx + 28) : (cx + 24);
        int rad  = (ch >= 88) ? 14 : 12;
        fill_circle32(bx_c, by_c, rad, cards[i].col);
        if (cards[i].glyph == '>') {
            draw_icon_play(bx_c + 1, by_c, 5, COL_WHITE);
        } else {
            int gh_c = 15;
            int gw_c = ipo_font_measure_glyph(IPO_FONT_BOLD, gh_c, (uint32_t)cards[i].glyph);
            if (gw_c < 1) gw_c = 9;
            ipo_font_draw_glyph_rgb32(s_fb32, s_gui_w, 0, 0, s_gui_w, s_gui_h,
                                      bx_c - gw_c / 2, by_c - gh_c / 2,
                                      IPO_FONT_BOLD, gh_c, (uint32_t)cards[i].glyph, COL_WHITE);
        }

        if (ch >= 88) {
            draw_text_styled32(cx + 52, cy + 15, cards[i].title, COL_TEXT_MAIN, cw - 62, IPO_FONT_BOLD, 15);
            draw_text_styled32(cx + 52, cy + 35, cards[i].tag,   COL_ACCENT_CYAN, cw - 62, IPO_FONT_REGULAR, ui_font_px());
            draw_text_styled32(cx + 16, cy + 70, cards[i].desc,  COL_TEXT_SUB, cw - 30, IPO_FONT_REGULAR, 12);
        } else {
            draw_text_styled32(cx + 44, cy + 8,  cards[i].title, COL_TEXT_MAIN, cw - 52, IPO_FONT_BOLD, 13);
            draw_text_styled32(cx + 44, cy + 25, cards[i].tag,   COL_ACCENT_CYAN, cw - 52, IPO_FONT_REGULAR, 11);
            if (ch >= 62) {
                draw_text_styled32(cx + 12, cy + ch - 19, cards[i].desc, COL_TEXT_SUB, cw - 22, IPO_FONT_REGULAR, 11);
            }
        }
    }
}

static void draw_video_player_viewport(ipo_tab_t *tab) {
    int vx = ipo_gui_vp_x();
    int vy = ipo_gui_vp_y();
    int vw = ipo_gui_vp_w();
    int vh = ipo_gui_vp_h();

    fill_rect32(vx, vy, vw, vh, 0x090D16u);

    video_layout_t V;
    compute_video_layout(vx, vy, vw, vh, &V);

    mp4_player_t *mp = s_mp4_player;
    const char *vtitle = (mp && mp4_player_title(mp)[0]) ? mp4_player_title(mp) : gui_str(tab->video_title);
    uint32_t dur_ms = mp ? mp4_player_duration_ms(mp) : 60000u;
    if (dur_ms == 0) dur_ms = 60000u;
    uint32_t cur_ms = (!s_vid_dragging_seek && mp) ? mp4_player_pos_ms(mp) : s_vid_pos_ms;
    if (cur_ms > dur_ms) cur_ms = dur_ms;

    if (V.compact) {
        int ch_f = ui_char_h();
        fill_rrect32(vx + V.pad, vy + 1, vw - 2 * V.pad, 14, 3, COL_CARD_BG);
        draw_icon_play(vx + V.pad + 7, vy + 8, 2, COL_ACCENT_RED);
        draw_text_ellipsis32(vx + V.pad + 14, vy + (14 - ch_f) / 2 + 1, vtitle, COL_TEXT_MAIN, vw - 2 * V.pad - V.back_w - 18);
        fill_rrect32(V.back_x, V.back_y, V.back_w, V.back_h, 3, (s_hover_id == 810) ? COL_BTN_HOVER : COL_OMNIBOX_BG);
        int bw_t = ui_text_w("< Back");
        draw_text_clip32(V.back_x + (V.back_w - bw_t) / 2, V.back_y + (V.back_h - ch_f) / 2, "< Back", COL_ACCENT_CYAN, V.back_w - 4, 1);
    } else {
        int ch_f = ui_char_h();
        int hdr_y = vy + 10;
        fill_rrect32(vx + V.pad, hdr_y, vw - 2 * V.pad, 34, 8, COL_CARD_BG);
        stroke_rrect32(vx + V.pad, hdr_y, vw - 2 * V.pad, 34, 8, COL_CARD_BD);

        fill_rrect32(vx + V.pad + 8, vy + 16, 88, 22, 6, COL_ACCENT_RED);
        draw_icon_play(vx + V.pad + 20, vy + 27, 4, COL_WHITE);
        draw_text_styled32(vx + V.pad + 30, vy + 16 + (22 - ch_f) / 2, "YouTube", COL_WHITE, 64, IPO_FONT_BOLD, ch_f);

        draw_text_ellipsis32(vx + V.pad + 106, hdr_y + (34 - ch_f) / 2, vtitle, COL_TEXT_MAIN, vw - 362);

        fill_rrect32(V.back_x, V.back_y, V.back_w, V.back_h, 6,
                     (s_hover_id == 810) ? COL_BTN_HOVER : COL_OMNIBOX_BG);
        stroke_rrect32(V.back_x, V.back_y, V.back_w, V.back_h, 6, COL_CARD_BD);
        {
            int bw_t = ui_text_w("< Back Page");
            int bx_t = V.back_x + (V.back_w - bw_t) / 2;
            if (bx_t < V.back_x + 4) bx_t = V.back_x + 4;
            draw_text_clip32(bx_t, V.back_y + (V.back_h - ch_f) / 2, "< Back Page", COL_ACCENT_CYAN, V.back_w - 8, 1);
        }
    }

    int vid_x = V.vid_x, vid_y = V.vid_y, vid_w = V.vid_w, vid_h = V.vid_h;

    stroke_rect32(vid_x - 1, vid_y - 1, vid_w + 2, vid_h + 2, COL_CARD_BD);
    fill_rect32(vid_x, vid_y, vid_w, vid_h, 0xFF000000u);

    int fw = 0, fh = 0, sw = 0, sh = 0;
    uint32_t fcnt = 0;
    const uint32_t *fpix = mp ? mp4_player_frame_pixels(mp, &fw, &fh, &sw, &sh, &fcnt) : NULL;
    if (fpix && fw > 0 && fh > 0) {
        if (fw == vid_w && fh == vid_h) {
            for (int y = 0; y < vid_h; y++) {
                const uint32_t *srow = fpix + y * fw;
                uint32_t *drow = s_fb32 + (vid_y + y) * IPO_GUI_W + vid_x;
                memcpy(drow, srow, (size_t)vid_w * sizeof(uint32_t));
            }
        } else {
            for (int y = 0; y < vid_h; y++) {
                int sy = (y * fh) / vid_h;
                const uint32_t *srow = fpix + sy * fw;
                uint32_t *drow = s_fb32 + (vid_y + y) * IPO_GUI_W + vid_x;
                for (int x = 0; x < vid_w; x++) {
                    drow[x] = (srow[(x * fw) / vid_w] & 0x00FFFFFFu) | IPO_PX_DITHER;
                }
            }
        }
    } else {
        const char *st_msg = mp ? mp4_player_status_text(mp) : "Initializing player...";
        int cx = vid_x + vid_w / 2;
        int cy_m = vid_y + vid_h / 2;
        int tw = ui_text_w(st_msg);
        if (tw > vid_w - 16) tw = vid_w - 16;
        fill_rrect32(vid_x + 12, cy_m - 18, vid_w - 24, 36, 6, 0x111827u);
        stroke_rrect32(vid_x + 12, cy_m - 18, vid_w - 24, 36, 6, COL_CARD_BD);
        draw_text_ellipsis32(cx - tw / 2, cy_m - ui_char_h() / 2, st_msg, COL_ACCENT_CYAN, vid_w - 32);
    }

    if (!V.compact) {
        char *codec_lbl = (sw > 0 && sh > 0)
                          ? gui_fmt_alloc("%dp H.264", sh)
                          : gui_fmt_alloc("H.264 MP4");

        int badge_ty = vid_y + 8 + (20 - ui_char_h()) / 2;
        fill_rrect32(vid_x + 10, vid_y + 8, 128, 20, 5, 0x111827u);
        fill_circle32(vid_x + 21, vid_y + 18, 4, s_vid_paused ? COL_ACCENT_GOLD : COL_ACCENT_RED);
        draw_text_clip32(vid_x + 30, badge_ty, s_vid_paused ? "PAUSED" : gui_str(codec_lbl), COL_TEXT_MAIN, 102, 1);
        free(codec_lbl);

        char *aud_lbl = s_vid_muted ? gui_fmt_alloc("Audio: MUTE")
                      : ((mp && mp4_player_has_audio(mp))
                         ? gui_fmt_alloc("AAC %dHz SB16", mp4_player_audio_rate(mp))
                         : gui_fmt_alloc("SB16 Audio"));
        fill_rrect32(vid_x + vid_w - 154, vid_y + 8, 144, 20, 5, 0x111827u);
        draw_text_clip32(vid_x + vid_w - 146, badge_ty, gui_str(aud_lbl),
                         s_vid_muted ? COL_ACCENT_GOLD : COL_ACCENT_GREEN, 132, 1);
        free(aud_lbl);
    }

    if (s_vid_paused) {
        int pcx = vid_x + vid_w / 2;
        int pcy = vid_y + vid_h / 2;
        int pr = V.compact ? 14 : 28;
        fill_circle32(pcx, pcy, pr, 0x0F172Au);
        stroke_rrect32(pcx - pr, pcy - pr, pr * 2, pr * 2, pr, COL_ACCENT_RED);
        draw_icon_play(pcx + 1, pcy, V.compact ? 6 : 11, COL_WHITE);
    }

    /* Player Control Panel below video */
    int cy = V.ctrl_y;
    fill_rrect32(vid_x, cy, vid_w, V.ctrl_h, V.compact ? 4 : 8, COL_CARD_BG);
    stroke_rrect32(vid_x, cy, vid_w, V.ctrl_h, V.compact ? 4 : 8, COL_CARD_BD);

    /* Scrubber / Seekbar */
    int sb_x = V.sb_x;
    int sb_y = V.sb_y;
    int sb_w = V.sb_w;
    fill_rrect32(sb_x, sb_y, sb_w, V.compact ? 3 : 6, 2, COL_CARD_BD);
    int fill_w = (int)(((uint64_t)sb_w * (uint64_t)cur_ms) / dur_ms);
    if (fill_w > sb_w) fill_w = sb_w;
    if (fill_w > 0) fill_rrect32(sb_x, sb_y, fill_w, V.compact ? 3 : 6, 2, COL_ACCENT_RED);
    fill_circle32(sb_x + fill_w, sb_y + (V.compact ? 1 : 3), V.compact ? 3 : 6, COL_WHITE);

    /* Control Buttons Row */
    int by = V.btn_y;
    int ty = by + (V.btn_h - ui_char_h()) / 2;
    fill_rrect32(V.play_x, by, V.play_w, V.btn_h, 4, (s_hover_id == 811) ? 0x3B82F6u : COL_ACCENT_BLUE);
    if (V.compact) {
        const char *pt = s_vid_paused ? "Play" : "Pause";
        int ptw = ui_text_w(pt);
        draw_text_clip32(V.play_x + (V.play_w - ptw) / 2, ty, pt, COL_WHITE, V.play_w - 4, 1);
    } else if (s_vid_paused) {
        draw_icon_play(V.play_x + 16, by + V.btn_h / 2, 4, COL_WHITE);
        draw_text_styled32(V.play_x + 28, ty, "Play", COL_WHITE, 56, IPO_FONT_BOLD, ui_char_h());
    } else {
        draw_icon_pause(V.play_x + 14, by + V.btn_h / 2, 4, COL_WHITE);
        draw_text_styled32(V.play_x + 25, ty, "Pause", COL_WHITE, 56, IPO_FONT_BOLD, ui_char_h());
    }

    fill_rrect32(V.b5_x, by, V.b5_w, V.btn_h, 4, (s_hover_id == 812) ? COL_BTN_HOVER : COL_OMNIBOX_BG);
    {
        int bw5 = ui_text_w("-5s");
        draw_text_clip32(V.b5_x + (V.b5_w - bw5) / 2, ty, "-5s", COL_TEXT_MAIN, V.b5_w - 4, 1);
    }

    fill_rrect32(V.f5_x, by, V.f5_w, V.btn_h, 4, (s_hover_id == 813) ? COL_BTN_HOVER : COL_OMNIBOX_BG);
    {
        int fw5 = ui_text_w("+5s");
        draw_text_clip32(V.f5_x + (V.f5_w - fw5) / 2, ty, "+5s", COL_TEXT_MAIN, V.f5_w - 4, 1);
    }

    fill_rrect32(V.mute_x, by, V.mute_w, V.btn_h, 4,
                 s_vid_muted ? COL_RED_DARK : ((s_hover_id == 814) ? COL_BTN_HOVER : COL_OMNIBOX_BG));
    {
        const char *ml = s_vid_muted ? (V.compact ? "Unmt" : "Unmute") : "Mute";
        int mw = ui_text_w(ml);
        draw_text_clip32(V.mute_x + (V.mute_w - mw) / 2, ty, ml, COL_TEXT_MAIN, V.mute_w - 4, 1);
    }

    /* Time display */
    unsigned cur_s = cur_ms / 1000u;
    unsigned tot_s = dur_ms / 1000u;
    char *tstr = V.compact
                 ? gui_fmt_alloc("%02u:%02u", cur_s / 60u, cur_s % 60u)
                 : gui_fmt_alloc("%02u:%02u / %02u:%02u",
                                 cur_s / 60u, cur_s % 60u, tot_s / 60u, tot_s % 60u);
    int time_x = V.compact ? (V.mute_x + V.mute_w + 6) : (V.vid_x + 320);
    if (tstr && time_x + 40 < V.full_x) {
        draw_text_clip32(time_x, ty, tstr, COL_TEXT_SUB, V.full_x - time_x - 4, 1);
    }
    free(tstr);

    /* Fullscreen button on right */
    fill_rrect32(V.full_x, by, V.full_w, V.btn_h, 4, (s_hover_id == 815) ? COL_BTN_HOVER : COL_OMNIBOX_BG);
    {
        const char *fl = V.compact ? (s_fullscreen ? "Win" : "Full") : (s_fullscreen ? "Exit F11" : "Full F11");
        int fw_t = ui_text_w(fl);
        draw_text_clip32(V.full_x + (V.full_w - fw_t) / 2, ty, fl, COL_TEXT_MAIN, V.full_w - 4, 1);
    }
}

static void draw_viewport_scrollbar(void) {
    if (!s_tabs || s_active_tab < 0 || s_active_tab >= s_tabs_cap) return;
    ipo_tab_t *tab = &s_tabs[s_active_tab];
    if (!tab->used || tab->is_home || tab->is_video || !tab->dev) return;
    struct session *ses = dev_to_session(tab->dev);
    if (!ses) return;
    struct f_data_c *fd = current_frame(ses);
    if (!fd || !fd->f_data || !fd->vs) return;
    int doc_h = fd->f_data->y;
    int win_h = fd->yw;
    if (doc_h <= win_h || win_h <= 0) return;

    int vx = ipo_gui_vp_x();
    int vy = ipo_gui_vp_y();
    int vw = ipo_gui_vp_w();
    int vh = ipo_gui_vp_h();
    int sb_x = vx + vw - 10;

    fill_rect32(sb_x, vy, 10, vh, 0x0F172Au);
    fill_rect32(sb_x, vy, 1, vh, COL_BORDER);

    int thumb_h = (vh * win_h) / doc_h;
    if (thumb_h < 28) thumb_h = 28;
    if (thumb_h > vh - 4) thumb_h = vh - 4;
    int max_scroll = doc_h - win_h;
    int pos = fd->vs->view_pos;
    if (pos < 0) pos = 0;
    if (pos > max_scroll) pos = max_scroll;
    int thumb_y = vy + 2 + ((vh - 4 - thumb_h) * pos) / (max_scroll > 0 ? max_scroll : 1);

    uint32_t tcol = (s_dragging_vscroll || s_hover_id == 950) ? COL_ACCENT_BLUE : 0x475569u;
    fill_rrect32(sb_x + 2, thumb_y, 6, thumb_h, 3, tcol);
}

static void scroll_viewport_to_mouse_y(int my) {
    if (!s_tabs || s_active_tab < 0 || s_active_tab >= s_tabs_cap) return;
    ipo_tab_t *tab = &s_tabs[s_active_tab];
    if (!tab->used || tab->is_home || tab->is_video || !tab->dev) return;
    struct session *ses = dev_to_session(tab->dev);
    if (!ses) return;
    struct f_data_c *fd = current_frame(ses);
    if (!fd || !fd->f_data || !fd->vs) return;
    int doc_h = fd->f_data->y;
    int win_h = fd->yw;
    if (doc_h <= win_h || win_h <= 0) return;

    int vy = ipo_gui_vp_y();
    int vh = ipo_gui_vp_h();
    int rel_y = my - vy;
    if (rel_y < 0) rel_y = 0;
    if (rel_y > vh) rel_y = vh;
    int max_scroll = doc_h - win_h;
    fd->vs->view_pos = (rel_y * max_scroll) / (vh > 0 ? vh : 1);
    fd->vs->orig_view_pos = fd->vs->view_pos;
    draw_graphical_doc(ses->term, fd, 1);
    s_dirty_vp = true;
}

static void draw_chrome(void) {
    int ui_ch = ui_char_h();
    if (s_fullscreen) {
        if (s_my < 38) {
            int ew = ui_text_w("Exit Full (F11)");
            int bw = (IPO_GUI_W >= 180) ? (ew + 16) : (IPO_GUI_W - 12);
            int bh = ui_is_compact_font() ? 14 : 22;
            fill_rrect32(IPO_GUI_W - bw - 6, 4, bw, bh, 4, COL_TABBAR_BG);
            stroke_rrect32(IPO_GUI_W - bw - 6, 4, bw, bh, 4, COL_CARD_BD);
            draw_text_clip32(IPO_GUI_W - 6 - (bw + ew) / 2, 4 + (bh - ui_ch) / 2, "Exit Full (F11)", COL_ACCENT_CYAN, bw - 8, 1);
        }
        return;
    }

    /* 1. Tab Bar */
    int tabbar_h = IPO_TABBAR_H;
    fill_rect32(0, 0, IPO_GUI_W, tabbar_h, COL_TABBAR_BG);
    int n_tabs = tab_count();
    if (n_tabs < 1) n_tabs = 1;

    tabbar_layout_t TL;
    compute_tabbar_layout_ex(n_tabs, &TL);

    if (s_tab_scroll > n_tabs - TL.max_vis) s_tab_scroll = n_tabs - TL.max_vis;
    if (s_tab_scroll < 0) s_tab_scroll = 0;

    bool overflow = (n_tabs > TL.max_vis);
    int tx = TL.tx0;
    if (overflow) {
        fill_rrect32(tx, TL.arrow_y, TL.arrow_w, TL.arrow_h, 3,
                     (s_hover_id == 148) ? COL_BTN_HOVER : COL_TAB_INACTIVE);
        draw_glyph32(tx + (TL.arrow_w - ui_glyph_adv('<')) / 2, TL.arrow_y + (TL.arrow_h - ui_ch) / 2, '<',
                     s_tab_scroll > 0 ? COL_TEXT_MAIN : COL_TEXT_DIM, 1);
        tx += TL.arrow_step;
    }

    int seen = 0, drawn = 0;
    for (int i = 0; i < s_tabs_cap && drawn < TL.vis_count; i++) {
        if (!s_tabs[i].used) continue;
        if (seen++ < s_tab_scroll) continue;
        drawn++;
        bool act = (i == s_active_tab);
        bool hov = (s_hover_id == 1000 + i);
        uint32_t bg = act ? COL_TAB_ACTIVE : (hov ? COL_TAB_HOVER : COL_TAB_INACTIVE);
        int tab_w = TL.tw - TL.tab_gap;
        fill_rrect32(tx, TL.tab_y, tab_w, TL.tab_h, TL.compact ? 3 : 5, bg);
        if (act) {
            fill_rect32(tx, TL.tab_y + TL.tab_h - 3, tab_w, 3, COL_TAB_ACTIVE);
            fill_rect32(tx + 4, TL.tab_y, tab_w - 8, TL.compact ? 1 : 2, COL_ACCENT_BLUE);
        }
        bool loading = is_tab_loading(i);
        uint32_t dot_col = loading ? (((gui_millis() / 220u) & 1u) ? COL_ACCENT_CYAN : COL_ACCENT_GOLD) :
                           (s_tabs[i].is_video ? COL_ACCENT_RED :
                            (s_tabs[i].is_home ? COL_ACCENT_CYAN : COL_ACCENT_GREEN));
        int dot_x = tx + (TL.compact ? 6 : 12);
        int dot_y = TL.tab_y + TL.tab_h / 2;
        fill_circle32(dot_x, dot_y, TL.compact ? 2 : 3, dot_col);
        int txt_x = tx + (TL.compact ? 11 : 20);
        int txt_y = TL.tab_y + (TL.tab_h - ui_ch) / 2;
        if (txt_y < 0) txt_y = 0;
        draw_text_ellipsis32(txt_x, txt_y, (s_tabs[i].title && s_tabs[i].title[0]) ? s_tabs[i].title : "Tab",
                             act ? COL_TEXT_MAIN : COL_TEXT_SUB, TL.tw - (TL.compact ? 22 : 42));

        if (s_hover_id == 2000 + i) {
            fill_circle32(tx + TL.tw - TL.close_off, dot_y, TL.compact ? 4 : 6, COL_ACCENT_RED);
        }
        draw_icon_close(tx + TL.tw - TL.close_off, dot_y, TL.compact ? 2 : 3,
                        act ? COL_TEXT_MAIN : COL_TEXT_SUB);
        tx += TL.tw;
    }

    if (overflow) {
        fill_rrect32(tx, TL.arrow_y, TL.arrow_w, TL.arrow_h, 3,
                     (s_hover_id == 149) ? COL_BTN_HOVER : COL_TAB_INACTIVE);
        draw_glyph32(tx + (TL.arrow_w - ui_glyph_adv('>')) / 2, TL.arrow_y + (TL.arrow_h - ui_ch) / 2, '>',
                     (s_tab_scroll + TL.max_vis < n_tabs) ? COL_TEXT_MAIN : COL_TEXT_DIM, 1);
        tx += TL.arrow_step;
    }

    /* New Tab [+] button */
    if (tx + TL.plus_w + 2 <= TL.f11_x - 2) {
        int pcx = tx + 2 + TL.plus_w / 2;
        int pcy = TL.plus_y + TL.plus_h / 2;
        int pr = TL.compact ? 3 : 4;
        fill_rrect32(tx + 2, TL.plus_y, TL.plus_w, TL.plus_h, 3,
                     (s_hover_id == 150) ? COL_BTN_HOVER : COL_TAB_INACTIVE);
        draw_line32(pcx - pr, pcy, pcx + pr, pcy, COL_TEXT_MAIN);
        draw_line32(pcx, pcy - pr, pcx, pcy + pr, COL_TEXT_MAIN);
    }

    /* Window Right Controls: [F11] & [Close X] */
    fill_rrect32(TL.f11_x, TL.ctrl_y, TL.ctrl_w, TL.ctrl_h, 3,
                 (s_hover_id == 160) ? COL_BTN_HOVER : COL_TAB_INACTIVE);
    int fsz = TL.compact ? 5 : 7;
    stroke_rect32(TL.f11_x + (TL.ctrl_w - fsz) / 2, TL.ctrl_y + (TL.ctrl_h - fsz) / 2, fsz, fsz, COL_TEXT_MAIN);
    fill_rrect32(TL.cls_x, TL.ctrl_y, TL.ctrl_w, TL.ctrl_h, 3,
                 (s_hover_id == 161) ? COL_ACCENT_RED : COL_RED_DARK);
    draw_icon_close(TL.cls_x + TL.ctrl_w / 2, TL.ctrl_y + TL.ctrl_h / 2, TL.compact ? 2 : 4, COL_WHITE);

    /* 2. Navigation & Omnibox Toolbar */
    int tool_h = IPO_TOOLBAR_H;
    fill_rect32(0, tabbar_h, IPO_GUI_W, tool_h, COL_TOOLBAR_BG);
    fill_rect32(0, tabbar_h + tool_h - 1, IPO_GUI_W, 1, COL_BORDER);

    bool can_back = can_tab_go_back(s_active_tab);
    bool can_fwd  = can_tab_go_forward(s_active_tab);
    bool act_load = is_tab_loading(s_active_tab);

    toolbar_layout_t BL;
    compute_toolbar_layout_ex(&BL);

    for (int b = 0; b < 4; b++) {
        int bx = BL.nav_x0 + b * BL.nav_step;
        int by = BL.oy;
        int cx = bx + BL.nav_w / 2;
        int cy = by + BL.oh / 2;
        fill_rrect32(bx, by, BL.nav_w, BL.oh, 4,
                     (s_hover_id == 200 + b) ? COL_BTN_HOVER : COL_TOOLBAR_BG);
        if (b == 0) draw_icon_back(cx, cy, can_back ? COL_TEXT_MAIN : COL_TEXT_DIM);
        else if (b == 1) draw_icon_fwd(cx, cy, can_fwd ? COL_TEXT_MAIN : COL_TEXT_DIM);
        else if (b == 2) {
            if (act_load) draw_icon_close(cx, cy, BL.compact ? 2 : 4, COL_ACCENT_RED);
            else draw_icon_reload(cx, cy, COL_TEXT_MAIN);
        }
        else draw_icon_home(cx, cy, COL_TEXT_MAIN);
    }

    /* Rounded Omnibox */
    int ox = BL.ox, oy = BL.oy, ow = BL.ow, oh = BL.oh;
    int dl_bx = BL.dl_x, hist_x = BL.hist_x, find_x = BL.find_x, vid_x = BL.vid_x, menu_x = BL.menu_x;
    fill_rrect32(ox, oy, ow, oh, BL.compact ? 4 : 7, COL_OMNIBOX_BG);
    stroke_rrect32(ox, oy, ow, oh, BL.compact ? 4 : 7,
                   (s_focus == FOCUS_OMNIBOX) ? COL_ACCENT_BLUE : COL_OMNIBOX_BD);

    ipo_tab_t *tab = &s_tabs[s_active_tab];
    bool is_https = (!tab->is_home && strncmp(gui_str(s_omnibox_text), "https://", 8) == 0);
    bool is_vid = tab->is_video;

    /* Clickable Security / Site Info Badge (hit = 208) */
    int badge_x = ox + (BL.compact ? 2 : 3);
    int badge_h = BL.compact ? (oh - 2) : (oh - 4);
    int badge_y = oy + (oh - badge_h) / 2;
    int badge_cx = badge_x + BL.badge_w / 2;
    int badge_cy = oy + oh / 2;
    if (is_https) {
        fill_rrect32(badge_x, badge_y, BL.badge_w, badge_h, 3,
                     (s_hover_id == 208) ? COL_ACCENT_GREEN : COL_GREEN_DARK);
        draw_icon_lock(badge_cx, badge_cy, (s_hover_id == 208) ? COL_BLACK : COL_ACCENT_GREEN);
    } else if (is_vid) {
        fill_rrect32(badge_x, badge_y, BL.badge_w, badge_h, 3, COL_RED_DARK);
        draw_icon_play(badge_cx - 1, badge_cy, 2, COL_ACCENT_RED);
    } else {
        fill_rrect32(badge_x, badge_y, BL.badge_w, badge_h, 3,
                     (s_hover_id == 208) ? COL_BTN_HOVER : COL_OMNIBOX_BG);
        fill_circle32(badge_cx, badge_cy, BL.compact ? 3 : 4, COL_TEXT_SUB);
        fill_circle32(badge_cx, badge_cy, 1, COL_OMNIBOX_BG);
    }

    int otxt_x, otxt_w, otxt_y;
    compute_omnibox_text_rect(&BL, &otxt_x, &otxt_w, &otxt_y);
    draw_text_scrolled32(otxt_x, otxt_y, gui_str(s_omnibox_text), s_omnibox_len,
                         s_omnibox_cursor, &s_omnibox_scroll,
                         s_focus == FOCUS_OMNIBOX, s_omnibox_sel_all,
                         COL_TEXT_MAIN, COL_TEXT_DIM,
                         BL.compact ? "Search / URL..." : "Search Google or enter URL...",
                         otxt_w, oy + 1, oh - 2);

    /* Bookmark Star inside Omnibox */
    const char *cur_u = tab->is_home ? "about:home" : (tab->is_video ? gui_str(tab->video_url) : gui_str(tab->url));
    bool starred = is_url_bookmarked(cur_u);
    int star_x = ox + ow - BL.star_w - (BL.compact ? 2 : 3);
    fill_rrect32(star_x, badge_y, BL.star_w, badge_h, 3,
                 (s_hover_id == 210) ? COL_BTN_HOVER : COL_OMNIBOX_BG);
    draw_icon_star(star_x + BL.star_w / 2, badge_cy, starred ? COL_ACCENT_GOLD : COL_TEXT_SUB, starred);

    /* Right Toolbar Action Buttons */
    int btn_ty = oy + (oh - ui_ch) / 2;
    int dl_active = 0, dl_any = 0;
    for (int i = 0; i < s_dls_cap; i++) {
        if (s_dls[i].used) {
            dl_any++;
            if (s_dls[i].state != DL_DONE && s_dls[i].state != DL_ERROR) dl_active++;
        }
    }
    if (BL.show_extra) {
        fill_rrect32(dl_bx, oy, BL.act_w, oh, 4,
                     (s_overlay == OVL_DOWNLOADS) ? COL_ACCENT_BLUE :
                     ((s_hover_id == 220) ? COL_BTN_HOVER : COL_TOOLBAR_BG));
        draw_icon_download(dl_bx + BL.act_w / 2, oy + oh / 2, COL_TEXT_MAIN);
        if (dl_any > 0) {
            fill_circle32(dl_bx + BL.act_w - 4, oy + 4, 2, dl_active > 0 ? COL_ACCENT_CYAN : COL_ACCENT_GREEN);
        }

        /* History [H] */
        fill_rrect32(hist_x, oy, BL.act_w, oh, 4,
                     (s_overlay == OVL_HISTORY) ? COL_ACCENT_BLUE :
                     ((s_hover_id == 224) ? COL_BTN_HOVER : COL_TOOLBAR_BG));
        draw_glyph32(hist_x + (BL.act_w - ui_glyph_adv('H')) / 2, btn_ty, 'H', COL_TEXT_MAIN, 1);

        /* Find [F] */
        fill_rrect32(find_x, oy, BL.act_w, oh, 4,
                     (s_overlay == OVL_FIND) ? COL_ACCENT_BLUE :
                     ((s_hover_id == 221) ? COL_BTN_HOVER : COL_TOOLBAR_BG));
        draw_glyph32(find_x + (BL.act_w - ui_glyph_adv('F')) / 2, btn_ty, 'F', COL_TEXT_MAIN, 1);
    }

    /* Video Player [>Y] */
    fill_rrect32(vid_x, oy, BL.vid_w, oh, 4,
                 tab->is_video ? COL_ACCENT_RED :
                 ((s_hover_id == 222) ? COL_BTN_HOVER : COL_TOOLBAR_BG));
    if (BL.compact) {
        draw_icon_play(vid_x + BL.vid_w / 2, oy + oh / 2, 3, tab->is_video ? COL_WHITE : COL_ACCENT_RED);
    } else {
        draw_icon_play(vid_x + 10, oy + oh / 2, 4, tab->is_video ? COL_WHITE : COL_ACCENT_RED);
        draw_glyph32(vid_x + 18, btn_ty, 'Y', COL_TEXT_MAIN, 1);
    }

    /* Browser Menu [=] */
    fill_rrect32(menu_x, oy, BL.act_w, oh, 4,
                 (s_overlay == OVL_MENU) ? COL_ACCENT_BLUE :
                 ((s_hover_id == 223) ? COL_BTN_HOVER : COL_TOOLBAR_BG));
    {
        int mw_b = BL.compact ? 7 : 10;
        int mh_s = BL.compact ? 3 : 4;
        int mt_h = BL.compact ? 1 : 2;
        int mx0 = menu_x + (BL.act_w - mw_b) / 2;
        int my0 = oy + oh / 2 - mh_s;
        fill_rect32(mx0, my0,            mw_b, mt_h, COL_TEXT_MAIN);
        fill_rect32(mx0, my0 + mh_s,     mw_b, mt_h, COL_TEXT_MAIN);
        fill_rect32(mx0, my0 + mh_s * 2, mw_b, mt_h, COL_TEXT_MAIN);
        if (!BL.show_extra && dl_any > 0) {
            fill_circle32(menu_x + BL.act_w - 2, oy + 2, 2, dl_active > 0 ? COL_ACCENT_CYAN : COL_ACCENT_GREEN);
        }
    }

    /* 3. Bookmarks Bar (shown in desktop mode; omitted cleanly in tight <= 279px mode) */
    int bm_h = IPO_BOOKMARKS_H;
    int bmy = tabbar_h + tool_h;
    if (bm_h > 0) {
        fill_rect32(0, bmy, IPO_GUI_W, bm_h, COL_BOOKMARK_BG);
        fill_rect32(0, bmy + bm_h - 1, IPO_GUI_W, 1, COL_BORDER);

        int bmx = 8;
        for (int i = 0; i < s_bookmark_count; i++) {
            const char *blbl = gui_str(s_bookmarks[i].label);
            int lw = ui_text_w(blbl) + 24;
            if (bmx + lw > IPO_GUI_W - 8) break;
            fill_rrect32(bmx, bmy + 2, lw, bm_h - 4, 4, (s_hover_id == 300 + i) ? COL_BTN_HOVER : COL_BOOKMARK_BG);
            fill_circle32(bmx + 9, bmy + bm_h / 2, 4, s_bookmarks[i].badge_color);
            draw_text_clip32(bmx + 18, bmy + (bm_h - ui_ch) / 2, blbl, COL_TEXT_SUB, lw - 20, 1);
            bmx += lw + 4;
        }
    }

    /* Animated Page Loading Progress Bar */
    if (act_load || dl_active > 0) {
        int prog_y = (bm_h > 0) ? (bmy + bm_h - 2) : (tabbar_h + tool_h - 2);
        int prog_x = (int)((gui_millis() / 3u) % (uint32_t)IPO_GUI_W);
        fill_rect32(0, prog_y, IPO_GUI_W, 2, 0x0F172Au);
        int bar_w = IPO_GUI_W / 3;
        for (int k = 0; k < bar_w; k++) {
            int px = (prog_x + k) % IPO_GUI_W;
            uint32_t c = lerp_color(COL_ACCENT_BLUE, COL_ACCENT_CYAN, (k * 256) / (bar_w > 0 ? bar_w : 1));
            fill_rect32(px, prog_y, 1, 2, c);
        }
    }

    /* 4. Bottom Status Bar */
    statusbar_layout_t SL;
    compute_statusbar_layout(&SL);
    int sby = SL.sby;
    fill_rect32(0, sby, IPO_GUI_W, SL.sbh, COL_STATUSBAR_BG);
    fill_rect32(0, sby, IPO_GUI_W, 1, COL_BORDER);

    const char *stat_txt = (s_status_msg && s_status_msg[0]) ? s_status_msg : "Ready";
    uint32_t stat_col = COL_TEXT_SUB;
    char *link_preview = NULL;
    if (!tab->is_home && !tab->is_video && tab->dev) {
        struct session *ses = dev_to_session(tab->dev);
        struct f_data_c *fd = ses ? current_frame(ses) : NULL;
        if (fd && fd->f_data && fd->vs &&
            fd->vs->current_link >= 0 && fd->vs->current_link < fd->f_data->nlinks) {
            struct link *lnk = &fd->f_data->links[fd->vs->current_link];
            if (lnk->where && lnk->where[0]) {
                link_preview = gui_fmt_alloc("Link: %s", (const char *)lnk->where);
                if (link_preview) {
                    stat_txt = link_preview;
                    stat_col = COL_ACCENT_CYAN;
                }
            }
        }
    }

    int st_x = SL.compact ? 4 : 8;
    int st_y = sby + (SL.sbh - ui_ch) / 2;
    draw_text_ellipsis32(st_x, st_y, stat_txt, stat_col, SL.zx - st_x - 6);
    free(link_preview);

    int zby = sby + (SL.compact ? 1 : 2);
    int zbh = SL.sbh - (SL.compact ? 2 : 4);
    int zty = sby + (SL.sbh - ui_ch) / 2;
    fill_rrect32(SL.zx, zby, SL.btn_w, zbh, 3, (s_hover_id == 410) ? COL_BTN_HOVER : COL_CARD_BG);
    draw_glyph32(SL.zx + (SL.btn_w - ui_glyph_adv('-')) / 2, zty, '-', COL_TEXT_MAIN, 1);

    char *zbuf = gui_fmt_alloc("%d%%", s_zoom_pct);
    fill_rrect32(SL.pct_x, zby, SL.pct_w, zbh, 3, (s_hover_id == 411) ? COL_BTN_HOVER : COL_OMNIBOX_BG);
    if (zbuf) {
        int ztw = ui_text_w(zbuf);
        draw_text_clip32(SL.pct_x + (SL.pct_w - ztw) / 2, zty, zbuf, s_zoom_pct == 100 ? COL_TEXT_SUB : COL_ACCENT_CYAN, SL.pct_w - 4, 1);
        free(zbuf);
    }

    fill_rrect32(SL.plus_x, zby, SL.btn_w, zbh, 3, (s_hover_id == 412) ? COL_BTN_HOVER : COL_CARD_BG);
    draw_glyph32(SL.plus_x + (SL.btn_w - ui_glyph_adv('+')) / 2, zty, '+', COL_TEXT_MAIN, 1);

    if (SL.show_hint) {
        char *right_stat = gui_fmt_alloc("Tabs:%d | Ctrl+H Hist | F11", n_tabs);
        if (right_stat) {
            int rsw = ui_text_w(right_stat);
            draw_text_clip32(IPO_GUI_W - rsw - 8, zty, right_stat, COL_TEXT_DIM, rsw, 1);
            free(right_stat);
        }
    }
}

static void draw_overlay(void) {
    int ui_cw = ui_char_w();
    int ui_ch = ui_char_h();
    bool compact_ui = (ui_is_compact_font() != 0);

    /* Live Omnibox Autocomplete Dropdown when Omnibox is focused */
    if (s_overlay == OVL_NONE) {
        if (s_focus == FOCUS_OMNIBOX && s_ac_count > 0 && !s_fullscreen) {
            int ax, ay, aw, ah, row_h, vis_count;
            compute_autocomplete_layout(&ax, &ay, &aw, &ah, &row_h, &vis_count);
            fill_rrect32(ax, ay, aw, ah, compact_ui ? 5 : 8, COL_TABBAR_BG);
            stroke_rrect32(ax, ay, aw, ah, compact_ui ? 5 : 8, COL_ACCENT_BLUE);
            int ty_off = (row_h - ui_ch) / 2;
            if (ty_off < 1) ty_off = 1;
            for (int i = 0; i < vis_count; i++) {
                int iy = ay + 3 + i * row_h;
                bool sel = (i == s_ac_sel) || (s_hover_id == 0x100000 + i);
                if (sel) fill_rrect32(ax + 4, iy, aw - 8, row_h - 2, 4, COL_CARD_HOVER);
                uint32_t bcol = s_ac_items[i].is_bookmark ? COL_ACCENT_GOLD :
                                (s_ac_items[i].is_search ? COL_ACCENT_BLUE : COL_ACCENT_CYAN);
                fill_circle32(ax + (compact_ui ? 10 : 15), iy + row_h / 2, compact_ui ? 2 : 4, bcol);
                int tx0 = ax + (compact_ui ? 18 : 26);
                const char *albl = gui_str(s_ac_items[i].label);
                const char *aurl = gui_str(s_ac_items[i].url);
                if (!s_ac_items[i].is_search && aw >= 340) {
                    int lw = (aw >= 460) ? 210 : (aw / 2 - 24);
                    draw_text_ellipsis32(tx0, iy + ty_off, albl, COL_TEXT_MAIN, lw);
                    draw_text_ellipsis32(tx0 + lw + 8, iy + ty_off, aurl, COL_TEXT_DIM, aw - (tx0 - ax) - lw - 14);
                } else {
                    draw_text_ellipsis32(tx0, iy + ty_off, albl, COL_TEXT_MAIN, aw - (tx0 - ax) - 6);
                }
            }
        }
        return;
    }

    if (s_overlay == OVL_FIND) {
        int fx, fy, fw, fh, param;
        compute_overlay_rect(OVL_FIND, &fx, &fy, &fw, &fh, &param);
        int in_x, in_w, prv_x, nxt_x, cls_x, nav_w, cls_w, btn_y, btn_h;
        compute_find_layout(fx, fy, fw, fh,
                            &in_x, &in_w, &prv_x, &nxt_x, &cls_x,
                            &nav_w, &cls_w, &btn_y, &btn_h);
        int ty = btn_y + (btn_h - ui_ch) / 2;

        fill_rrect32(fx, fy, fw, fh, compact_ui ? 5 : 8, COL_TABBAR_BG);
        stroke_rrect32(fx, fy, fw, fh, compact_ui ? 5 : 8, COL_ACCENT_BLUE);
        fill_rrect32(in_x, btn_y, in_w, btn_h, 4, COL_OMNIBOX_BG);
        stroke_rrect32(in_x, btn_y, in_w, btn_h, 4, COL_OMNIBOX_BD);
        draw_text_scrolled32(in_x + 5, ty, gui_str(s_find_text), s_find_len,
                             s_find_cursor, &s_find_scroll,
                             s_focus == FOCUS_FIND, s_find_sel_all,
                             COL_TEXT_MAIN, COL_TEXT_DIM, "Find in page...",
                             in_w - 10, btn_y + 1, btn_h - 2);
        fill_rrect32(prv_x, btn_y, nav_w, btn_h, 4, (s_hover_id == 601) ? COL_BTN_HOVER : COL_CARD_BG);
        draw_glyph32(prv_x + (nav_w - ui_glyph_adv('<')) / 2, ty, '<', COL_TEXT_MAIN, 1);
        fill_rrect32(nxt_x, btn_y, nav_w, btn_h, 4, (s_hover_id == 602) ? COL_BTN_HOVER : COL_CARD_BG);
        draw_glyph32(nxt_x + (nav_w - ui_glyph_adv('>')) / 2, ty, '>', COL_TEXT_MAIN, 1);
        fill_rrect32(cls_x, btn_y, cls_w, btn_h, 4, (s_hover_id == 603) ? COL_ACCENT_RED : COL_CARD_BG);
        draw_icon_close(cls_x + cls_w / 2, btn_y + btn_h / 2, compact_ui ? 2 : 3, COL_TEXT_MAIN);
    } else if (s_overlay == OVL_MENU) {
        menu_layout_t M;
        compute_menu_layout(&M);
        fill_rrect32(M.mx, M.my, M.mw, M.mh, M.compact ? 5 : 8, COL_TABBAR_BG);
        stroke_rrect32(M.mx, M.my, M.mw, M.mh, M.compact ? 5 : 8, COL_CARD_BD);

        /* Interactive Zoom Row at top of Menu */
        draw_text_clip32(M.mx + (M.compact ? 8 : 14), M.z_y + (M.z_h - ui_ch) / 2, "Zoom", COL_TEXT_SUB, 48, 1);
        fill_rrect32(M.minus_x, M.z_y, M.minus_w, M.z_h, 4, (s_hover_id == 720) ? COL_BTN_HOVER : COL_CARD_BG);
        draw_glyph32(M.minus_x + (M.minus_w - ui_glyph_adv('-')) / 2, M.z_y + (M.z_h - ui_ch) / 2, '-', COL_TEXT_MAIN, 1);
        char *zstr = gui_fmt_alloc("%d%%", s_zoom_pct);
        fill_rrect32(M.pct_x, M.z_y, M.pct_w, M.z_h, 4, (s_hover_id == 721) ? COL_BTN_HOVER : COL_OMNIBOX_BG);
        if (zstr) {
            draw_text_clip32(M.pct_x + (M.pct_w - ui_text_w(zstr)) / 2, M.z_y + (M.z_h - ui_ch) / 2,
                             zstr, COL_ACCENT_CYAN, M.pct_w - 4, 1);
            free(zstr);
        }
        fill_rrect32(M.plus_x, M.z_y, M.plus_w, M.z_h, 4, (s_hover_id == 722) ? COL_BTN_HOVER : COL_CARD_BG);
        draw_glyph32(M.plus_x + (M.plus_w - ui_glyph_adv('+')) / 2, M.z_y + (M.z_h - ui_ch) / 2, '+', COL_TEXT_MAIN, 1);
        fill_rect32(M.mx + 8, M.sep_y, M.mw - 16, 1, COL_BORDER);

        static const struct {
            const char *label;
            const char *short_label;
            int id;
        } items[9] = {
            { "New Tab         Ctrl+T", "New Tab    Ctrl+T", 700 },
            { "Close Tab       Ctrl+W", "Close Tab  Ctrl+W", 701 },
            { "History         Ctrl+H", "History    Ctrl+H", 708 },
            { "Downloads       Ctrl+J", "Downloads  Ctrl+J", 702 },
            { "Find in Page    Ctrl+F", "Find       Ctrl+F", 703 },
            { "Bookmark Page   Ctrl+D", "Bookmark   Ctrl+D", 704 },
            { "Fullscreen         F11", "Fullscreen    F11", 705 },
            { "Links Core Menu    F10", "Links Menu    F10", 706 },
            { "Exit Browser    Ctrl+Q", "Exit       Ctrl+Q", 707 }
        };
        if (M.two_col) {
            int item_h = (M.row_step > 2) ? (M.row_step - 1) : M.row_step;
            for (int i = 0; i < 9; i++) {
                int col = (i < 4) ? 0 : 1;
                int row = (i < 4) ? i : (i - 4);
                int ix = M.mx + 6 + col * (M.col_w + 6);
                int iy = M.row_y0 + row * M.row_step;
                bool hov = (s_hover_id == items[i].id);
                if (hov) fill_rrect32(ix, iy, M.col_w, item_h, 3, items[i].id == 707 ? COL_ACCENT_RED : COL_ACCENT_BLUE);
                draw_text_clip32(ix + 4, iy + (item_h - ui_ch) / 2, items[i].short_label,
                                 (hov || items[i].id == 707) ? COL_TEXT_MAIN : COL_TEXT_SUB, M.col_w - 6, 1);
            }
        } else {
            int item_h = M.row_step - 3;
            if (item_h < ui_ch + 2) item_h = ui_ch + 2;
            int text_off = (item_h - ui_ch) / 2;
            if (text_off < 0) text_off = 0;
            for (int i = 0; i < 9; i++) {
                int iy = M.row_y0 + i * M.row_step;
                if (iy + item_h > M.my + M.mh - 2) break;
                bool hov = (s_hover_id == items[i].id);
                if (hov) fill_rrect32(M.mx + 6, iy, M.mw - 12, item_h, 5, items[i].id == 707 ? COL_ACCENT_RED : COL_ACCENT_BLUE);
                draw_text_clip32(M.mx + 14, iy + text_off, items[i].label,
                                 (hov || items[i].id == 707) ? COL_TEXT_MAIN : COL_TEXT_SUB, M.mw - 24, 1);
            }
        }
    } else if (s_overlay == OVL_SECURITY) {
        int sx, sy, sw, sh, param;
        compute_overlay_rect(OVL_SECURITY, &sx, &sy, &sw, &sh, &param);
        fill_rrect32(sx, sy, sw, sh, compact_ui ? 6 : 10, COL_TABBAR_BG);
        stroke_rrect32(sx, sy, sw, sh, compact_ui ? 6 : 10, COL_ACCENT_BLUE);

        ipo_tab_t *tab = &s_tabs[s_active_tab];
        const char *u = tab->is_home ? "about:home" : (tab->is_video ? gui_str(tab->video_url) : gui_str(tab->url));
        bool https = (!tab->is_home && strncmp(u, "https://", 8) == 0);
        uint32_t badge_c = https ? COL_ACCENT_GREEN : (tab->is_home ? COL_ACCENT_CYAN : COL_ACCENT_GOLD);
        const char *sec_title = https ? "Connection is Secure (HTTPS / TLS)" :
                                (tab->is_home ? "Internal Browser Start Page" :
                                 (tab->is_video ? "Media Stream Playback" : "Unencrypted HTTP Connection"));

        int hdr_h = compact_ui ? 22 : 36;
        int cls_s = compact_ui ? 14 : 22;
        fill_circle32(sx + (compact_ui ? 12 : 20), sy + hdr_h / 2, compact_ui ? 4 : 6, badge_c);
        draw_text_clip32(sx + (compact_ui ? 22 : 34), sy + (hdr_h - ui_ch) / 2, sec_title, badge_c, sw - 54, 1);
        fill_rrect32(sx + sw - cls_s - 6, sy + (hdr_h - cls_s) / 2, cls_s, cls_s, 4,
                     (s_hover_id == 520) ? COL_ACCENT_RED : COL_CARD_BG);
        draw_icon_close(sx + sw - 6 - cls_s / 2, sy + hdr_h / 2, compact_ui ? 2 : 3, COL_TEXT_MAIN);
        fill_rect32(sx + 8, sy + hdr_h, sw - 16, 1, COL_BORDER);

        int y0 = sy + hdr_h + (compact_ui ? 4 : 6);
        int line_h = compact_ui ? 12 : ((sh >= 168) ? 22 : 16);
        if (y0 + ui_ch <= sy + sh - 4) {
            draw_text_ellipsis32(sx + 10, y0, (tab->title && tab->title[0]) ? tab->title : "Untitled", COL_TEXT_MAIN, sw - 20);
        }
        if (y0 + line_h + ui_ch <= sy + sh - 4) {
            draw_text_ellipsis32(sx + 10, y0 + line_h, u[0] ? u : "about:home", COL_ACCENT_CYAN, sw - 20);
        }

        int n_links = 0, n_imgs = 0, doc_y = 0;
        struct session *ses = dev_to_session(tab->dev);
        struct f_data_c *fd = ses ? current_frame(ses) : NULL;
        if (fd && fd->f_data) {
            n_links = fd->f_data->nlinks;
            n_imgs  = fd->f_data->n_images;
            doc_y   = fd->f_data->y;
        }
        char *meta1 = gui_fmt_alloc("Links: %d  |  Images: %d  |  Height: %dpx", n_links, n_imgs, doc_y);
        char *meta2 = gui_fmt_alloc("Encoding: UTF-8  |  Page Zoom: %d%%", s_zoom_pct);
        int m_off = compact_ui ? (line_h * 2 + 2) : ((sh >= 168) ? 56 : (line_h * 2));
        if (meta1 && y0 + m_off + ui_ch <= sy + sh - 4) {
            draw_text_clip32(sx + 10, y0 + m_off, meta1, COL_TEXT_SUB, sw - 20, 1);
        }
        if (meta2 && y0 + m_off + line_h + ui_ch <= sy + sh - 4) {
            draw_text_clip32(sx + 10, y0 + m_off + line_h, meta2, COL_TEXT_SUB, sw - 20, 1);
        }
        free(meta1);
        free(meta2);
        if (!compact_ui && sh >= 168) {
            draw_text_clip32(sx + 14, sy + 152, "Verified by Links OpenSSL 1.1 TLS Stack", COL_TEXT_DIM, sw - 28, 1);
        } else if (y0 + m_off + line_h * 2 + ui_ch <= sy + sh - 4) {
            draw_text_clip32(sx + 10, y0 + m_off + line_h * 2, "Links BearSSL TLS Stack", COL_TEXT_DIM, sw - 20, 1);
        }
    } else if (s_overlay == OVL_HISTORY) {
        int hx, hy, hw, hh, max_items;
        compute_overlay_rect(OVL_HISTORY, &hx, &hy, &hw, &hh, &max_items);
        fill_rrect32(hx, hy, hw, hh, compact_ui ? 6 : 10, COL_TABBAR_BG);
        stroke_rrect32(hx, hy, hw, hh, compact_ui ? 6 : 10, COL_CARD_BD);

        int hdr_h = compact_ui ? 20 : 36;
        int row_step = compact_ui ? 22 : 36;
        int cls_s = compact_ui ? 14 : 22;
        int clr_w = compact_ui ? 60 : 80;
        draw_glyph32(hx + 10, hy + (hdr_h - ui_ch) / 2, 'H', COL_ACCENT_CYAN, 1);
        draw_text_clip32(hx + 10 + ui_cw + 6, hy + (hdr_h - ui_ch) / 2, "Browsing History", COL_TEXT_MAIN, hw - clr_w - cls_s - 36, 1);
        fill_rrect32(hx + hw - cls_s - clr_w - 12, hy + (hdr_h - cls_s) / 2, clr_w, cls_s, 4,
                     (s_hover_id == 560) ? COL_ACCENT_RED : COL_CARD_BG);
        draw_text_clip32(hx + hw - cls_s - clr_w - 6, hy + (hdr_h - ui_ch) / 2, "Clear All", COL_TEXT_MAIN, clr_w - 8, 1);
        fill_rrect32(hx + hw - cls_s - 6, hy + (hdr_h - cls_s) / 2, cls_s, cls_s, 4,
                     (s_hover_id == 559) ? COL_ACCENT_RED : COL_CARD_BG);
        draw_icon_close(hx + hw - 6 - cls_s / 2, hy + hdr_h / 2, compact_ui ? 2 : 3, COL_TEXT_MAIN);
        fill_rect32(hx + 8, hy + hdr_h, hw - 16, 1, COL_BORDER);

        int list_y = hy + hdr_h + (compact_ui ? 2 : 6);
        if (s_history_count == 0) {
            const char *msg = "No browsing history recorded yet";
            int msg_tw = hw - 24;
            int msg_w = ui_text_w(msg);
            int msg_x = (msg_w < msg_tw) ? (hx + (hw - msg_w) / 2) : (hx + 12);
            int msg_y = list_y + (hy + hh - list_y - ui_ch) / 2;
            draw_text_clip32(msg_x, msg_y, msg, COL_TEXT_SUB, msg_tw, 1);
        } else {
            int shown = 0;
            int card_h = row_step - (compact_ui ? 2 : 3);
            for (int i = s_history_count - 1; i >= 0 && shown < max_items; i--, shown++) {
                int iy = list_y + shown * row_step;
                bool hov = (s_hover_id == 0x200000 + shown);
                fill_rrect32(hx + 8, iy, hw - 16, card_h, 4, hov ? COL_CARD_HOVER : COL_CARD_BG);
                stroke_rrect32(hx + 8, iy, hw - 16, card_h, 4, hov ? COL_ACCENT_BLUE : COL_CARD_BD);
                draw_text_ellipsis32(hx + 14, iy + (compact_ui ? 2 : 2), gui_str(s_history[i].title), COL_TEXT_MAIN, hw - 28);
                draw_text_ellipsis32(hx + 14, iy + (compact_ui ? 11 : 16), gui_str(s_history[i].url), COL_ACCENT_CYAN, hw - 28);
            }
        }
    } else if (s_overlay == OVL_CONTEXT) {
        int cx, cy, cw, ch, row_h, pad_y, n_items;
        compute_context_layout(&cx, &cy, &cw, &ch, &row_h, &pad_y, &n_items);
        (void)n_items;

        fill_rrect32(cx, cy, cw, ch, compact_ui ? 5 : 8, COL_TABBAR_BG);
        stroke_rrect32(cx, cy, cw, ch, compact_ui ? 5 : 8, COL_ACCENT_BLUE);

        int text_off = (row_h - ui_ch) / 2;
        if (text_off < 0) text_off = 0;
        int row = 0;
        if (s_ctx_has_link) {
            static const char *l_items[3] = {
                "Open Link in New Tab",
                "Copy Link Address",
                "Download Link (/data/)"
            };
            for (int i = 0; i < 3; i++, row++) {
                int iy = cy + pad_y + row * row_h;
                bool hov = (s_hover_id == 750 + i);
                if (hov) fill_rrect32(cx + 5, iy, cw - 10, row_h - 2, 4, COL_ACCENT_BLUE);
                draw_text_clip32(cx + 10, iy + text_off, l_items[i], hov ? COL_WHITE : COL_ACCENT_CYAN, cw - 16, 1);
            }
            fill_rect32(cx + 8, cy + pad_y + row * row_h - 1, cw - 16, 1, COL_BORDER);
        }
        static const char *p_items[7] = {
            "Back            Alt+Left",
            "Forward        Alt+Right",
            "Reload Page       Ctrl+R",
            "Bookmark Page     Ctrl+D",
            "Save Page (/data/)",
            "Find in Page      Ctrl+F",
            "Page & Security Info"
        };
        for (int i = 0; i < 7; i++, row++) {
            int iy = cy + pad_y + row * row_h;
            bool hov = (s_hover_id == 753 + i);
            if (hov) fill_rrect32(cx + 5, iy, cw - 10, row_h - 2, 4, COL_ACCENT_BLUE);
            draw_text_clip32(cx + 10, iy + text_off, p_items[i], hov ? COL_WHITE : COL_TEXT_MAIN, cw - 16, 1);
        }
    } else if (s_overlay == OVL_DOWNLOADS) {
        int dx, dy, dw, dh, max_slots;
        compute_overlay_rect(OVL_DOWNLOADS, &dx, &dy, &dw, &dh, &max_slots);
        fill_rrect32(dx, dy, dw, dh, compact_ui ? 6 : 10, COL_TABBAR_BG);
        stroke_rrect32(dx, dy, dw, dh, compact_ui ? 6 : 10, COL_CARD_BD);

        bool compact_d = (compact_ui || dh < 130);
        int hdr_y = compact_ui ? (dy + 18) : (compact_d ? (dy + 28) : (dy + 36));
        int cls_s = compact_ui ? 14 : 20;

        /* Header */
        draw_icon_download(dx + (compact_ui ? 12 : 20), dy + (hdr_y - dy) / 2, COL_ACCENT_CYAN);
        draw_text_clip32(dx + (compact_ui ? 22 : 34), dy + ((hdr_y - dy) - ui_ch) / 2,
                         "Downloads (/data/)", COL_TEXT_MAIN, dw - 56, 1);
        fill_rrect32(dx + dw - cls_s - 6, dy + ((hdr_y - dy) - cls_s) / 2, cls_s, cls_s, 4,
                     (s_hover_id == 500) ? COL_ACCENT_RED : COL_CARD_BG);
        draw_icon_close(dx + dw - 6 - cls_s / 2, dy + (hdr_y - dy) / 2, compact_ui ? 2 : 3, COL_TEXT_MAIN);
        fill_rect32(dx + 8, hdr_y, dw - 16, 1, COL_BORDER);

        /* List active / completed download slots */
        int shown = 0;
        int iy = hdr_y + (compact_ui ? 3 : 4);
        int row_step = compact_ui ? 26 : 52;
        int card_h = compact_ui ? 24 : 46;
        int prog_h = compact_ui ? 5 : 8;
        int prog_off = compact_ui ? 16 : 30;
        for (int i = s_dls_cap - 1; i >= 0 && shown < max_slots; i--) {
            dl_slot_t *dl = &s_dls[i];
            if (!dl->used) continue;
            shown++;
            fill_rrect32(dx + 8, iy, dw - 16, card_h, 5, COL_CARD_BG);
            stroke_rrect32(dx + 8, iy, dw - 16, card_h, 5, COL_CARD_BD);

            char *st = NULL;
            uint32_t st_col = COL_ACCENT_CYAN;
            if (dl->state == DL_DONE) {
                st = gui_fmt_alloc("%lu B OK", (unsigned long)dl->downloaded_bytes);
                st_col = COL_ACCENT_GREEN;
            } else if (dl->state == DL_ERROR) {
                st = gui_fmt_alloc("%s", (dl->err_msg && dl->err_msg[0]) ? dl->err_msg : "Error");
                st_col = COL_ACCENT_RED;
            } else {
                st = gui_fmt_alloc("%lu B", (unsigned long)dl->downloaded_bytes);
            }
            const char *st_s = gui_str(st);
            int stw = ui_text_w(st_s);
            draw_text_ellipsis32(dx + 14, iy + (compact_ui ? 3 : 6),
                                 dl->dest ? dl->dest : "", COL_TEXT_MAIN, dw - 36 - stw);
            draw_text_clip32(dx + dw - 14 - stw, iy + (compact_ui ? 3 : 6), st_s, st_col, stw + 2, 1);
            free(st);

            /* Progress track */
            int px = dx + 14, py = iy + prog_off, pw = dw - 28;
            fill_rrect32(px, py, pw, prog_h, 3, COL_OMNIBOX_BG);
            if (dl->state == DL_DONE) {
                fill_rrect32(px, py, pw, prog_h, 3, COL_ACCENT_GREEN);
            } else if (dl->state == DL_ERROR) {
                fill_rrect32(px, py, pw / 3, prog_h, 3, COL_ACCENT_RED);
            } else if (dl->content_length > 0) {
                int fw = (int)(((uint64_t)pw * dl->downloaded_bytes) / (uint64_t)dl->content_length);
                if (fw > pw) fw = pw;
                if (fw > 0) fill_rrect32(px, py, fw, prog_h, 3, COL_ACCENT_BLUE);
            } else if (pw > 44) {
                int offset = (int)((gui_millis() / 18u) % (uint32_t)(pw - 40));
                fill_rrect32(px + offset, py, 44, prog_h, 3, COL_ACCENT_CYAN);
            }
            iy += row_step;
        }

        /* Also list Links core downloads if any */
        struct download *ld_entry;
        struct list_head *ld_it;
        foreachback(struct download, ld_entry, ld_it, downloads) {
            if (shown >= max_slots) break;
            shown++;
            fill_rrect32(dx + 8, iy, dw - 16, card_h, 5, COL_CARD_BG);
            stroke_rrect32(dx + 8, iy, dw - 16, card_h, 5, COL_CARD_BD);
            char *lb = gui_fmt_alloc("%ld B", (long)ld_entry->last_pos);
            const char *lb_s = gui_str(lb);
            int lbw = ui_text_w(lb_s);
            draw_text_ellipsis32(dx + 14, iy + (compact_ui ? 3 : 6),
                                 ld_entry->file ? (const char *)ld_entry->file : "Links download",
                                 COL_TEXT_MAIN, dw - 36 - lbw);
            draw_text_clip32(dx + dw - 14 - lbw, iy + (compact_ui ? 3 : 6), lb_s, COL_ACCENT_GREEN, lbw + 2, 1);
            free(lb);
            fill_rrect32(dx + 14, iy + prog_off, dw - 28, prog_h, 3,
                         (ld_entry->stat.state < 0) ? COL_ACCENT_GREEN : COL_ACCENT_BLUE);
            iy += row_step;
        }

        int by, btn_h, bx0, bw, bstep;
        compute_downloads_btn_layout(dx, dy, dw, dh, &by, &btn_h, &bx0, &bw, &bstep);

        if (shown == 0) {
            int msg_y = hdr_y + ((by - 4 - hdr_y) - ui_ch) / 2;
            draw_text_clip32(dx + 14, msg_y, "No active downloads yet", COL_TEXT_SUB, dw - 28, 1);
        }

        /* Bottom Action Buttons inside Drawer */
        int bty = by + (btn_h - ui_ch) / 2;
        fill_rect32(dx + 8, by - 4, dw - 16, 1, COL_BORDER);
        fill_rrect32(bx0, by, bw, btn_h, 4, (s_hover_id == 501) ? 0x3B82F6u : COL_ACCENT_BLUE);
        draw_text_clip32(bx0 + 4, bty, "DL Cur Page", COL_WHITE, bw - 8, 1);

        fill_rrect32(bx0 + bstep, by, bw, btn_h, 4, (s_hover_id == 502) ? COL_BTN_HOVER : COL_CARD_BG);
        stroke_rrect32(bx0 + bstep, by, bw, btn_h, 4, COL_CARD_BD);
        draw_text_clip32(bx0 + bstep + 4, bty, "example.com", COL_ACCENT_CYAN, bw - 8, 1);

        fill_rrect32(bx0 + bstep * 2, by, bw, btn_h, 4, (s_hover_id == 503) ? COL_BTN_HOVER : COL_CARD_BG);
        stroke_rrect32(bx0 + bstep * 2, by, bw, btn_h, 4, COL_CARD_BD);
        draw_text_clip32(bx0 + bstep * 2 + 4, bty, "google.com", COL_ACCENT_GREEN, bw - 8, 1);
    }
}

/* =========================================================================
 * Compositor Present & WM Fallback
 * ========================================================================= */
static void dispatch_scancode(uint8_t raw_sc);

static const char *const s_cur_arrow[16] = {
    "#...........",
    "##..........",
    "#@#.........",
    "#@@#........",
    "#@@@#.......",
    "#@@@@#......",
    "#@@@@@#.....",
    "#@@@@@@#....",
    "#@@@@@@@#...",
    "#@@@@@@@@#..",
    "#@@@@@#####.",
    "#@@#@@#.....",
    "#@#.#@@#....",
    "##..#@@#....",
    ".....##.....",
    "............"
};
static const char *const s_cur_hand[16] = {
    "....##........",
    "...#@@#.......",
    "...#@@#.......",
    "...#@@#.......",
    "...#@@###.....",
    "...#@@#@@##...",
    ".###@@#@@#@@#.",
    "#@@#@@#@@#@@#.",
    "#@@@@@@@@@@@@#",
    "#@@@@@@@@@@@@#",
    ".#@@@@@@@@@@#.",
    "..#@@@@@@@@@#.",
    "..#@@@@@@@@#..",
    "...#@@@@@@#...",
    "...########...",
    ".............."
};
static const char *const s_cur_ibeam[16] = {
    "#######",
    "#@@@@@#",
    "##.@.##",
    "..#@#..",
    "..#@#..",
    "..#@#..",
    "..#@#..",
    "..#@#..",
    "..#@#..",
    "..#@#..",
    "..#@#..",
    "..#@#..",
    "..#@#..",
    "##.@.##",
    "#@@@@@#",
    "#######"
};

static uint8_t s_sw_cur_save[24 * 24];
static uint32_t s_sw_cur_save32[24 * 24];
static int s_last_flip_mx = -1;
static int s_last_flip_my = -1;
static cursor_kind_t s_last_flip_cur = CURSOR_ARROW;

static int s_wm_cur_x0 = 0, s_wm_cur_y0 = 0, s_wm_cur_w = 0, s_wm_cur_h = 0;

static void draw_sw_cursor_on_buf32(uint32_t *buf, int bw, int bh,
                                    int *out_x0, int *out_y0, int *out_w, int *out_h) {
    *out_w = 0;
    *out_h = 0;
    if (!buf || bw <= 0 || bh <= 0) return;

    const char *const *shape = s_cur_arrow;
    int sw = 12, sh = 16, hx = 0, hy = 0;
    if (s_cursor == CURSOR_HAND) {
        shape = s_cur_hand; sw = 14; sh = 16; hx = 4; hy = 0;
    } else if (s_cursor == CURSOR_IBEAM) {
        shape = s_cur_ibeam; sw = 7; sh = 16; hx = 3; hy = 7;
    }

    int x0 = s_mx - hx;
    int y0 = s_my - hy;
    int cx1 = (x0 < 0) ? 0 : x0;
    int cy1 = (y0 < 0) ? 0 : y0;
    int cx2 = (x0 + sw > bw) ? bw : (x0 + sw);
    int cy2 = (y0 + sh > bh) ? bh : (y0 + sh);
    if (cx1 >= cx2 || cy1 >= cy2) return;

    int rw = cx2 - cx1;
    int rh = cy2 - cy1;
    *out_x0 = cx1; *out_y0 = cy1; *out_w = rw; *out_h = rh;

    for (int py = 0; py < rh; py++) {
        uint32_t *row = buf + (size_t)(cy1 + py) * (size_t)bw + cx1;
        uint32_t *sav = s_sw_cur_save32 + (size_t)py * (size_t)rw;
        const char *pat = shape[(cy1 + py) - y0];
        for (int px = 0; px < rw; px++) {
            sav[px] = row[px];
            char ch = pat[(cx1 + px) - x0];
            if (ch == '#') row[px] = 0xFF000000u;
            else if (ch == '@') row[px] = 0xFFFFFFFFu;
        }
    }
}

static void restore_sw_cursor32(int x0, int y0, int rw, int rh) {
    if (!s_fb32 || rw <= 0 || rh <= 0) return;
    for (int py = 0; py < rh; py++) {
        uint32_t *row = s_fb32 + (size_t)(y0 + py) * (size_t)s_gui_w + x0;
        const uint32_t *sav = s_sw_cur_save32 + (size_t)py * (size_t)rw;
        memcpy(row, sav, (size_t)rw * sizeof(uint32_t));
    }
}

static void draw_sw_cursor_on_buf(uint8_t *buf, int bw, int bh,
                                  int *out_x0, int *out_y0, int *out_w, int *out_h,
                                  bool save_under) {
    *out_w = 0;
    *out_h = 0;
    if (!buf || bw <= 0 || bh <= 0) return;

    const char *const *shape = s_cur_arrow;
    int sw = 12, sh = 16, hx = 0, hy = 0;
    if (s_cursor == CURSOR_HAND) {
        shape = s_cur_hand; sw = 14; sh = 16; hx = 4; hy = 0;
    } else if (s_cursor == CURSOR_IBEAM) {
        shape = s_cur_ibeam; sw = 7; sh = 16; hx = 3; hy = 7;
    }

    int mx = (bw == s_gui_w) ? s_mx : ((s_mx * bw) / (s_gui_w > 0 ? s_gui_w : 1));
    int my = (bh == s_gui_h) ? s_my : ((s_my * bh) / (s_gui_h > 0 ? s_gui_h : 1));
    int x0 = mx - hx;
    int y0 = my - hy;
    int cx1 = (x0 < 0) ? 0 : x0;
    int cy1 = (y0 < 0) ? 0 : y0;
    int cx2 = (x0 + sw > bw) ? bw : (x0 + sw);
    int cy2 = (y0 + sh > bh) ? bh : (y0 + sh);
    if (cx1 >= cx2 || cy1 >= cy2) return;

    int rw = cx2 - cx1;
    int rh = cy2 - cy1;
    *out_x0 = cx1; *out_y0 = cy1; *out_w = rw; *out_h = rh;

    uint8_t col_blk = lut_lookup_rgb(0, 0, 0);
    uint8_t col_wht = lut_lookup_rgb(255, 255, 255);

    for (int py = 0; py < rh; py++) {
        uint8_t *row = buf + (size_t)(cy1 + py) * (size_t)bw + cx1;
        uint8_t *sav = save_under ? (s_sw_cur_save + (size_t)py * (size_t)rw) : NULL;
        const char *pat = shape[(cy1 + py) - y0];
        for (int px = 0; px < rw; px++) {
            if (sav) sav[px] = row[px];
            char ch = pat[(cx1 + px) - x0];
            if (ch == '#') row[px] = col_blk;
            else if (ch == '@') row[px] = col_wht;
        }
    }
}

static void restore_sw_cursor(int x0, int y0, int rw, int rh) {
    if (!s_fb8_base || rw <= 0 || rh <= 0) return;
    for (int py = 0; py < rh; py++) {
        uint8_t *row = s_fb8_base + (size_t)(y0 + py) * (size_t)s_gui_w + x0;
        const uint8_t *sav = s_sw_cur_save + (size_t)py * (size_t)rw;
        memcpy(row, sav, (size_t)rw);
    }
}

static void blit_window_frame(wm_window_t *win);

static void restore_wm_cursor_from_base(wm_window_t *win) {
    if (!win || !win->framebuf || !s_fb8_base || s_wm_cur_w <= 0 || s_wm_cur_h <= 0) return;
    int ww = (int)win->w;
    int wh = (int)win->h;
    if (ww != s_gui_w || wh != s_gui_h) {
        blit_window_frame(win);
        s_wm_cur_w = 0;
        s_wm_cur_h = 0;
        return;
    }
    int x0 = s_wm_cur_x0, y0 = s_wm_cur_y0, rw = s_wm_cur_w, rh = s_wm_cur_h;
    if (x0 < 0) { rw += x0; x0 = 0; }
    if (y0 < 0) { rh += y0; y0 = 0; }
    if (x0 + rw > ww) rw = ww - x0;
    if (y0 + rh > wh) rh = wh - y0;
    if (rw > 0 && rh > 0) {
        for (int py = 0; py < rh; py++) {
            size_t off = (size_t)(y0 + py) * (size_t)ww + (size_t)x0;
            memcpy(win->framebuf + off, s_fb8_base + off, (size_t)rw);
        }
    }
    s_wm_cur_w = 0;
    s_wm_cur_h = 0;
}

static void poll_wm_events(void) {
    if (!s_wm_win) {
        if (system_is_interrupted()) {
            terminate_loop = 1;
            return;
        }
        keyboard_poll();
        for (;;) {
            uint8_t raw_sc = keyboard_get_scancode();
            if (raw_sc == 0) break;
            s_links_wakeup = true;
            dispatch_scancode(raw_sc);
            if (terminate_loop) return;
        }
        return;
    }

    if (!wm_is_window_valid(s_wm_win)) {
        terminate_loop = 1;
        return;
    }

    wm_event_t ev;
    while (wm_poll_event(s_wm_win, &ev)) {
        s_links_wakeup = true;
        if (ev.type == WM_EVENT_CLOSE) {
            terminate_loop = 1;
            return;
        }
        if (ev.type == WM_EVENT_KEY_DOWN || ev.type == WM_EVENT_KEY_UP) {
            dispatch_scancode((uint8_t)(ev.data & 0xFFu));
            if (terminate_loop) return;
        }
    }
}

static void blit_window_frame(wm_window_t *win) {
    if (!win || !win->framebuf || !s_fb8_base) return;
    int ww = (int)win->w;
    int wh = (int)win->h;
    int gw = s_gui_w;
    int gh = s_gui_h;
    if (ww <= 0 || wh <= 0 || gw <= 0 || gh <= 0) return;

    s_wm_last_w = ww;
    s_wm_last_h = wh;

    if (ww == gw && wh == gh) {
        memcpy(win->framebuf, s_fb8_base, (size_t)ww * (size_t)wh);
        return;
    }

    if (!s_wm_lut_x || s_wm_lut_x_w != ww || s_wm_lut_src_w != gw) {
        int *nlut = (int *)realloc(s_wm_lut_x, (size_t)ww * sizeof(int));
        if (nlut) {
            s_wm_lut_x = nlut;
            s_wm_lut_x_w = ww;
            s_wm_lut_src_w = gw;
            for (int x = 0; x < ww; x++) {
                s_wm_lut_x[x] = (x * gw) / ww;
            }
        }
    }

    int prev_sy = -1;
    uint8_t *prev_dst = NULL;
    for (int y = 0; y < wh; y++) {
        int sy = (y * gh) / wh;
        uint8_t *dst = win->framebuf + y * ww;
        if (sy == prev_sy && prev_dst) {
            memcpy(dst, prev_dst, (size_t)ww);
            continue;
        }
        const uint8_t *src = s_fb8_base + sy * gw;
        if (s_wm_lut_x && s_wm_lut_x_w == ww) {
            for (int x = 0; x < ww; x++) {
                dst[x] = src[s_wm_lut_x[x]];
            }
        } else {
            for (int x = 0; x < ww; x++) {
                dst[x] = src[(x * gw) / ww];
            }
        }
        prev_sy = sy;
        prev_dst = dst;
    }
}

static void flip_output(bool cursor_only) {
    if (!s_inited || s_shutting_down) return;
    if (s_use_truecolor32 && s_fb32) {
        if (s_wm_win && wm_is_window_valid(s_wm_win)) {
            if (cursor_only) {
                wm_compositor_tick();
                s_last_flip_mx = s_mx;
                s_last_flip_my = s_my;
                s_last_flip_cur = s_cursor;
                return;
            }
            ipo_gfx_surface_desc_t surf;
            memset(&surf, 0, sizeof(surf));
            surf.struct_size = (uint32_t)sizeof(surf);
            surf.width = (uint32_t)s_gui_w;
            surf.height = (uint32_t)s_gui_h;
            surf.stride_bytes = (uint32_t)s_gui_w * (uint32_t)sizeof(uint32_t);
            surf.format_id = IPO_COLOR_FMT_XRGB8888;
            surf.tonemap_op = IPO_TONEMAP_NONE;
            surf.exposure = 1.0;
            surf.pixels = s_fb32;
            ipo_gfx_present_window(s_wm_win, &surf);
            s_force_hw_reflip = false;
            s_last_flip_mx = s_mx;
            s_last_flip_my = s_my;
            s_last_flip_cur = s_cursor;
            return;
        }
        if (s_direct_gfx) {
            int cx0 = 0, cy0 = 0, cw = 0, ch = 0;
            draw_sw_cursor_on_buf32(s_fb32, s_gui_w, s_gui_h, &cx0, &cy0, &cw, &ch);
            ipo_gfx_surface_desc_t surf;
            memset(&surf, 0, sizeof(surf));
            surf.struct_size = (uint32_t)sizeof(surf);
            surf.width = (uint32_t)s_gui_w;
            surf.height = (uint32_t)s_gui_h;
            surf.stride_bytes = (uint32_t)s_gui_w * (uint32_t)sizeof(uint32_t);
            surf.format_id = IPO_COLOR_FMT_XRGB8888;
            surf.tonemap_op = IPO_TONEMAP_NONE;
            surf.exposure = 1.0;
            surf.pixels = s_fb32;
            ipo_gfx_present_screen(&surf);
            restore_sw_cursor32(cx0, cy0, cw, ch);
            s_last_flip_mx = s_mx;
            s_last_flip_my = s_my;
            s_last_flip_cur = s_cursor;
            return;
        }
    }
    if (!s_fb8_base) return;
    if (s_wm_win && wm_is_window_valid(s_wm_win)) {
        if (s_hw_direct_fb && wm_get_focused() == s_wm_win) {
            /* Present crisp 1:1 native resolution directly to BGA LFB while WM is
             * quiescent (do not wm_invalidate on normal frames so the 320x200
             * compositor does not overwrite VRAM on the next yield). */
            int cx0 = 0, cy0 = 0, cw = 0, ch = 0;
            draw_sw_cursor_on_buf(s_fb8_base, s_gui_w, s_gui_h, &cx0, &cy0, &cw, &ch, true);
            vga_gfx_flip_scaled_buffer(s_fb8_base, s_gui_w, s_gui_h);
            restore_sw_cursor(cx0, cy0, cw, ch);
            if (!cursor_only) {
                blit_window_frame(s_wm_win);
                s_wm_cur_w = 0;
                s_wm_cur_h = 0;
            }
            s_force_hw_reflip = false;
            s_last_flip_mx = s_mx;
            s_last_flip_my = s_my;
            s_last_flip_cur = s_cursor;
            return;
        }
        if (cursor_only) {
            restore_wm_cursor_from_base(s_wm_win);
        } else {
            blit_window_frame(s_wm_win);
            s_wm_cur_w = 0;
            s_wm_cur_h = 0;
        }
        draw_sw_cursor_on_buf(s_wm_win->framebuf, (int)s_wm_win->w, (int)s_wm_win->h,
                              &s_wm_cur_x0, &s_wm_cur_y0, &s_wm_cur_w, &s_wm_cur_h, false);
        wm_invalidate(s_wm_win);
        s_force_hw_reflip = false;
        s_last_flip_mx = s_mx;
        s_last_flip_my = s_my;
        s_last_flip_cur = s_cursor;
        return;
    }
    if (!s_direct_gfx) return;
    if (!cursor_only) apply_direct_ui_palette();
    int cx0 = 0, cy0 = 0, cw = 0, ch = 0;
    draw_sw_cursor_on_buf(s_fb8_base, s_gui_w, s_gui_h, &cx0, &cy0, &cw, &ch, true);
    vga_gfx_flip_scaled_buffer(s_fb8_base, s_gui_w, s_gui_h);
    restore_sw_cursor(cx0, cy0, cw, ch);
    s_last_flip_mx = s_mx;
    s_last_flip_my = s_my;
    s_last_flip_cur = s_cursor;
}

static uint32_t *s_ovl_save32 = NULL;
static size_t s_ovl_save_cap = 0;

static void render_and_present(void) {
    if (!s_inited || s_shutting_down || !s_fb32 || !s_fb8_base || !s_tabs) return;
    ipo_tab_t *tab = &s_tabs[s_active_tab];

    /* Track previously painted overlay / autocomplete rect and keep clean
     * chrome/viewport pixels in s_fb32 by restoring saved pixels right after
     * quantizing the overlay, so closing or shrinking an overlay never leaves
     * stale ghosts or strips Bayer dithering from adjacent images. */
    static int s_drawn_ovl_kind = -1; /* -1=none, 0=autocomplete, >0=overlay_kind_t */
    static int s_drawn_ovl_x = 0, s_drawn_ovl_y = 0, s_drawn_ovl_w = 0, s_drawn_ovl_h = 0;

    bool want_popup = (s_overlay != OVL_NONE);
    bool want_ac    = (!want_popup && s_focus == FOCUS_OMNIBOX && s_ac_count > 0 && !s_fullscreen);
    int want_kind   = want_popup ? (int)s_overlay : (want_ac ? 0 : -1);
    int cur_ox = 0, cur_oy = 0, cur_ow = 0, cur_oh = 0;
    if (want_popup) {
        int op;
        compute_overlay_rect(s_overlay, &cur_ox, &cur_oy, &cur_ow, &cur_oh, &op);
    } else if (want_ac) {
        int rh, vc;
        compute_autocomplete_layout(&cur_ox, &cur_oy, &cur_ow, &cur_oh, &rh, &vc);
    }
    int pad_x1 = 0, pad_y1 = 0, pad_x2 = 0, pad_y2 = 0;
    if (want_kind != -1) {
        pad_x1 = (cur_ox > 2) ? (cur_ox - 2) : 0;
        pad_y1 = (cur_oy > 2) ? (cur_oy - 2) : 0;
        pad_x2 = (cur_ox + cur_ow + 2 < s_gui_w) ? (cur_ox + cur_ow + 2) : s_gui_w;
        pad_y2 = (cur_oy + cur_oh + 2 < s_gui_h) ? (cur_oy + cur_oh + 2) : s_gui_h;
    }

    if (s_drawn_ovl_kind != -1) {
        int old_x1 = s_drawn_ovl_x;
        int old_y1 = s_drawn_ovl_y;
        int old_x2 = old_x1 + s_drawn_ovl_w;
        int old_y2 = old_y1 + s_drawn_ovl_h;
        bool geom_changed = (want_kind != s_drawn_ovl_kind) ||
                            (pad_x1 != old_x1) || (pad_y1 != old_y1) ||
                            (pad_x2 != old_x2) || (pad_y2 != old_y2);
        if (geom_changed) {
            if (!s_dirty_vp) {
                int vy = ipo_gui_vp_y();
                int vh = ipo_gui_vp_h();
                int vy2 = vy + vh;
                bool vp_dither = (tab && tab->used && !tab->is_home);
                if (old_x1 < 0) old_x1 = 0;
                if (old_y1 < 0) old_y1 = 0;
                if (old_x2 > s_gui_w) old_x2 = s_gui_w;
                if (old_y2 > s_gui_h) old_y2 = s_gui_h;
                if (old_x1 < old_x2 && old_y1 < old_y2) {
                    if (old_y1 < vy) {
                        int cy2 = (old_y2 < vy) ? old_y2 : vy;
                        if (old_y1 < cy2) quantize_rect(old_x1, old_y1, old_x2, cy2, false);
                    }
                    {
                        int iy1 = (old_y1 > vy) ? old_y1 : vy;
                        int iy2 = (old_y2 < vy2) ? old_y2 : vy2;
                        if (iy1 < iy2) quantize_rect(old_x1, iy1, old_x2, iy2, vp_dither);
                    }
                    if (old_y2 > vy2) {
                        int by1 = (old_y1 > vy2) ? old_y1 : vy2;
                        if (by1 < old_y2) quantize_rect(old_x1, by1, old_x2, old_y2, false);
                    }
                }
            }
            s_drawn_ovl_kind = -1;
            if (want_kind != -1) s_dirty_overlay = true;
        }
    }

    if (s_dirty_vp || s_dirty_vp_rows) {
        int vy = ipo_gui_vp_y();
        int vh = ipo_gui_vp_h();
        if (tab->used && tab->is_home) {
            draw_start_page();
            quantize_rows(vy, vy + vh, false);
        } else if (tab->used && tab->is_video) {
            draw_video_player_viewport(tab);
            quantize_rows(vy, vy + vh, true);
        } else if (tab->used) {
            if (s_dirty_vp) {
                draw_viewport_scrollbar();
                quantize_rows(vy, vy + vh, true);
            } else {
                int qy1 = (s_dirty_vp_y1 < vy) ? vy : s_dirty_vp_y1;
                int qy2 = (s_dirty_vp_y2 > vy + vh) ? (vy + vh) : s_dirty_vp_y2;
                if (qy1 < qy2) {
                    quantize_rows(qy1, qy2, true);
                }
                draw_viewport_scrollbar();
                quantize_rect(ipo_gui_vp_x() + ipo_gui_vp_w() - 10, vy,
                              ipo_gui_vp_x() + ipo_gui_vp_w(), vy + vh, false);
            }
        }
        s_dirty_vp = false;
        s_dirty_vp_rows = false;
        s_dirty_vp_y1 = s_gui_h;
        s_dirty_vp_y2 = 0;
        s_dirty_overlay = true;
    }

    if (s_dirty_chrome) {
        draw_chrome();
        if (!s_fullscreen) {
            quantize_rows(0, IPO_TOP_CHROME, false);
            quantize_rows(IPO_GUI_H - IPO_STATUSBAR_H, IPO_GUI_H, false);
        } else {
            quantize_rows(0, 40, false);
        }
        s_dirty_chrome = false;
        if (want_kind != -1) s_dirty_overlay = true;
    }

    if (s_dirty_overlay && want_kind != -1 && pad_x1 < pad_x2 && pad_y1 < pad_y2) {
        int sw = pad_x2 - pad_x1;
        int sh = pad_y2 - pad_y1;
        size_t need = (size_t)sw * (size_t)sh;
        if (need > s_ovl_save_cap) {
            uint32_t *nb = (uint32_t *)malloc(need * sizeof(uint32_t));
            if (nb) {
                free(s_ovl_save32);
                s_ovl_save32 = nb;
                s_ovl_save_cap = need;
            }
        }
        if (s_ovl_save32 && need <= s_ovl_save_cap) {
            for (int row = 0; row < sh; row++) {
                memcpy(s_ovl_save32 + (size_t)row * (size_t)sw,
                       s_fb32 + (size_t)(pad_y1 + row) * (size_t)s_gui_w + pad_x1,
                       (size_t)sw * sizeof(uint32_t));
            }
        }
        draw_overlay();
        quantize_rect(pad_x1, pad_y1, pad_x2, pad_y2, false);
        if (s_ovl_save32 && need <= s_ovl_save_cap) {
            for (int row = 0; row < sh; row++) {
                memcpy(s_fb32 + (size_t)(pad_y1 + row) * (size_t)s_gui_w + pad_x1,
                       s_ovl_save32 + (size_t)row * (size_t)sw,
                       (size_t)sw * sizeof(uint32_t));
            }
        }
        s_drawn_ovl_kind = want_kind;
        s_drawn_ovl_x = pad_x1;
        s_drawn_ovl_y = pad_y1;
        s_drawn_ovl_w = sw;
        s_drawn_ovl_h = sh;
        s_dirty_overlay = false;
    } else if (s_dirty_overlay) {
        s_dirty_overlay = false;
    }

    flip_output(false);
    s_last_present_ms = gui_millis();

    {
        static uint32_t s_fps_last_ms = 0;
        static uint32_t s_fps_frames = 0;
        extern int serial_printf(const char *fmt, ...);
        uint32_t now = s_last_present_ms;
        if (s_fps_last_ms == 0u) {
            s_fps_last_ms = now;
        }
        s_fps_frames++;
        uint32_t elapsed = now - s_fps_last_ms;
        if (elapsed >= 1000u) {
            uint32_t fps = (s_fps_frames * 1000u) / elapsed;
            serial_printf("[links] %u FPS (video=%d)\n", fps, (tab && tab->used && tab->is_video) ? 1 : 0);
            s_fps_frames = 0;
            s_fps_last_ms = now;
        }
    }
}

/* =========================================================================
 * Hover, Click & Wheel Handling
 * ========================================================================= */
static int hit_test_ui(int mx, int my, cursor_kind_t *out_cur) {
    *out_cur = CURSOR_ARROW;
    if (!s_tabs) return 0;

    /* Overlays & Autocomplete Dropdown take priority over underlying viewport/bars */
    if (s_overlay == OVL_CONTEXT) {
        int cx, cy, cw, ch, row_h, pad_y, n_items;
        compute_context_layout(&cx, &cy, &cw, &ch, &row_h, &pad_y, &n_items);
        if (mx >= cx && mx < cx + cw && my >= cy && my < cy + ch) {
            int row = (my - (cy + pad_y)) / row_h;
            if (row >= 0 && row < n_items) {
                *out_cur = CURSOR_HAND;
                if (s_ctx_has_link) return 750 + row;
                return 753 + row;
            }
            return 799;
        }
    } else if (s_overlay == OVL_FIND) {
        int fx, fy, fw, fh, param;
        compute_overlay_rect(OVL_FIND, &fx, &fy, &fw, &fh, &param);
        if (mx >= fx && mx < fx + fw && my >= fy && my < fy + fh) {
            int in_x, in_w, prv_x, nxt_x, cls_x, nav_w, cls_w, btn_y, btn_h;
            compute_find_layout(fx, fy, fw, fh,
                                &in_x, &in_w, &prv_x, &nxt_x, &cls_x,
                                &nav_w, &cls_w, &btn_y, &btn_h);
            (void)in_x; (void)in_w; (void)btn_y; (void)btn_h;
            if (mx >= cls_x && mx < cls_x + cls_w) { *out_cur = CURSOR_HAND; return 603; }
            if (mx >= nxt_x && mx < nxt_x + nav_w) { *out_cur = CURSOR_HAND; return 602; }
            if (mx >= prv_x && mx < prv_x + nav_w) { *out_cur = CURSOR_HAND; return 601; }
            *out_cur = CURSOR_IBEAM;
            return 600;
        }
    } else if (s_overlay == OVL_MENU) {
        menu_layout_t M;
        compute_menu_layout(&M);
        if (mx >= M.mx && mx < M.mx + M.mw && my >= M.my && my < M.my + M.mh) {
            if (my >= M.z_y && my < M.z_y + M.z_h) {
                if (mx >= M.minus_x && mx < M.minus_x + M.minus_w) { *out_cur = CURSOR_HAND; return 720; }
                if (mx >= M.pct_x   && mx < M.pct_x + M.pct_w)     { *out_cur = CURSOR_HAND; return 721; }
                if (mx >= M.plus_x  && mx < M.plus_x + M.plus_w)   { *out_cur = CURSOR_HAND; return 722; }
            }
            static const int menu_ids[9] = { 700, 701, 708, 702, 703, 704, 705, 706, 707 };
            if (M.two_col && my >= M.row_y0 && M.row_step > 0) {
                int row = (my - M.row_y0) / M.row_step;
                int col = (mx >= M.mx + 6 + M.col_w + 3) ? 1 : 0;
                int idx = (col == 0) ? row : (4 + row);
                if (col == 0 && row >= 0 && row < 4) { *out_cur = CURSOR_HAND; return menu_ids[idx]; }
                if (col == 1 && row >= 0 && row < 5) { *out_cur = CURSOR_HAND; return menu_ids[idx]; }
            } else if (!M.two_col && my >= M.row_y0 && M.row_step > 0) {
                int idx = (my - M.row_y0) / M.row_step;
                if (idx >= 0 && idx < 9) { *out_cur = CURSOR_HAND; return menu_ids[idx]; }
            }
            return 799;
        }
    } else if (s_overlay == OVL_SECURITY) {
        int sx, sy, sw, sh, param;
        compute_overlay_rect(OVL_SECURITY, &sx, &sy, &sw, &sh, &param);
        if (mx >= sx && mx < sx + sw && my >= sy && my < sy + sh) {
            int hdr_h = ui_is_compact_font() ? 22 : 36;
            if (mx >= sx + sw - 28 && my < sy + hdr_h) { *out_cur = CURSOR_HAND; return 520; }
            return 599;
        }
    } else if (s_overlay == OVL_HISTORY) {
        int hx, hy, hw, hh, max_items;
        compute_overlay_rect(OVL_HISTORY, &hx, &hy, &hw, &hh, &max_items);
        if (mx >= hx && mx < hx + hw && my >= hy && my < hy + hh) {
            bool compact_ui = (ui_is_compact_font() != 0);
            int hdr_h = compact_ui ? 20 : 36;
            int row_step = compact_ui ? 22 : 36;
            int cls_s = compact_ui ? 14 : 22;
            int clr_w = compact_ui ? 60 : 80;
            if (mx >= hx + hw - cls_s - 8 && my < hy + hdr_h) { *out_cur = CURSOR_HAND; return 559; }
            if (mx >= hx + hw - cls_s - clr_w - 14 && mx < hx + hw - cls_s - 8 && my < hy + hdr_h) {
                *out_cur = CURSOR_HAND; return 560;
            }
            int list_y = hy + hdr_h + (compact_ui ? 2 : 6);
            if (my >= list_y && s_history_count > 0) {
                int row = (my - list_y) / row_step;
                if (row >= 0 && row < max_items && row < s_history_count) {
                    *out_cur = CURSOR_HAND;
                    return 0x200000 + row;
                }
            }
            return 599;
        }
    } else if (s_overlay == OVL_DOWNLOADS) {
        int dx, dy, dw, dh, max_slots;
        compute_overlay_rect(OVL_DOWNLOADS, &dx, &dy, &dw, &dh, &max_slots);
        if (mx >= dx && mx < dx + dw && my >= dy && my < dy + dh) {
            int hdr_h = ui_is_compact_font() ? 18 : 36;
            if (mx >= dx + dw - 28 && my < dy + hdr_h) { *out_cur = CURSOR_HAND; return 500; }
            int by, btn_h, bx0, bw, bstep;
            compute_downloads_btn_layout(dx, dy, dw, dh, &by, &btn_h, &bx0, &bw, &bstep);
            if (my >= by && my < by + btn_h) {
                if (mx >= bx0             && mx < bx0 + bw)             { *out_cur = CURSOR_HAND; return 501; }
                if (mx >= bx0 + bstep     && mx < bx0 + bstep + bw)     { *out_cur = CURSOR_HAND; return 502; }
                if (mx >= bx0 + bstep * 2 && mx < bx0 + bstep * 2 + bw) { *out_cur = CURSOR_HAND; return 503; }
            }
            return 599;
        }
    } else if (s_focus == FOCUS_OMNIBOX && s_ac_count > 0 && !s_fullscreen) {
        int ax, ay, aw, ah, row_h, vis_count;
        compute_autocomplete_layout(&ax, &ay, &aw, &ah, &row_h, &vis_count);
        if (mx >= ax && mx < ax + aw && my >= ay && my < ay + ah) {
            int row = (my - (ay + 3)) / row_h;
            if (row >= 0 && row < vis_count) {
                *out_cur = CURSOR_HAND;
                return 0x100000 + row;
            }
        }
    }

    if (s_fullscreen) {
        int bw = (IPO_GUI_W >= 180) ? (ui_text_w("Exit Full (F11)") + 16) : (IPO_GUI_W - 12);
        int bh = ui_is_compact_font() ? 14 : 22;
        if (my < 4 + bh + 6 && mx >= IPO_GUI_W - bw - 8 && mx <= IPO_GUI_W - 4) {
            *out_cur = CURSOR_HAND;
            return 160;
        }
    } else {
        /* Tab Bar */
        if (my < IPO_TABBAR_H) {
            int n_tabs = tab_count();
            if (n_tabs < 1) n_tabs = 1;
            tabbar_layout_t TL;
            compute_tabbar_layout_ex(n_tabs, &TL);

            if (mx >= TL.f11_x && mx < TL.f11_x + TL.ctrl_w &&
                my >= TL.ctrl_y && my < TL.ctrl_y + TL.ctrl_h) {
                *out_cur = CURSOR_HAND; return 160;
            }
            if (mx >= TL.cls_x && mx < TL.cls_x + TL.ctrl_w &&
                my >= TL.ctrl_y && my < TL.ctrl_y + TL.ctrl_h) {
                *out_cur = CURSOR_HAND; return 161;
            }

            bool overflow = (n_tabs > TL.max_vis);
            int tx = TL.tx0;
            if (overflow) {
                if (mx >= tx && mx < tx + TL.arrow_w &&
                    my >= TL.arrow_y && my < TL.arrow_y + TL.arrow_h) {
                    *out_cur = CURSOR_HAND; return 148;
                }
                tx += TL.arrow_step;
            }

            int seen = 0, drawn = 0;
            for (int i = 0; i < s_tabs_cap && drawn < TL.vis_count; i++) {
                if (!s_tabs[i].used) continue;
                if (seen++ < s_tab_scroll) continue;
                drawn++;
                if (mx >= tx && mx < tx + TL.tw - TL.tab_gap && my >= TL.tab_y) {
                    *out_cur = CURSOR_HAND;
                    if (mx >= tx + TL.tw - TL.close_hit_off) return 2000 + i;
                    return 1000 + i;
                }
                tx += TL.tw;
            }
            if (overflow) {
                if (mx >= tx && mx < tx + TL.arrow_w &&
                    my >= TL.arrow_y && my < TL.arrow_y + TL.arrow_h) {
                    *out_cur = CURSOR_HAND; return 149;
                }
                tx += TL.arrow_step;
            }
            if (tx + TL.plus_w + 2 <= TL.f11_x - 2 &&
                mx >= tx + 2 && mx < tx + 2 + TL.plus_w &&
                my >= TL.plus_y && my < TL.plus_y + TL.plus_h) {
                *out_cur = CURSOR_HAND;
                return 150;
            }
            return 0;
        }
        /* Toolbar */
        if (my >= IPO_TABBAR_H && my < IPO_TABBAR_H + IPO_TOOLBAR_H) {
            toolbar_layout_t BL;
            compute_toolbar_layout_ex(&BL);
            if (my >= BL.oy && my < BL.oy + BL.oh) {
                for (int b = 0; b < 4; b++) {
                    int bx = BL.nav_x0 + b * BL.nav_step;
                    if (mx >= bx && mx < bx + BL.nav_w) { *out_cur = CURSOR_HAND; return 200 + b; }
                }
                if (mx >= BL.ox && mx < BL.ox + BL.ow) {
                    if (mx < BL.ox + BL.badge_w + 5) { *out_cur = CURSOR_HAND; return 208; }
                    if (mx >= BL.ox + BL.ow - BL.star_w - 5) { *out_cur = CURSOR_HAND; return 210; }
                    *out_cur = CURSOR_IBEAM;
                    return 209;
                }
                if (BL.show_extra) {
                    if (mx >= BL.dl_x   && mx < BL.dl_x + BL.act_w)   { *out_cur = CURSOR_HAND; return 220; }
                    if (mx >= BL.hist_x && mx < BL.hist_x + BL.act_w) { *out_cur = CURSOR_HAND; return 224; }
                    if (mx >= BL.find_x && mx < BL.find_x + BL.act_w) { *out_cur = CURSOR_HAND; return 221; }
                }
                if (mx >= BL.vid_x  && mx < BL.vid_x + BL.vid_w)  { *out_cur = CURSOR_HAND; return 222; }
                if (mx >= BL.menu_x && mx < BL.menu_x + BL.act_w) { *out_cur = CURSOR_HAND; return 223; }
            }
            return 0;
        }
        /* Bookmarks Bar */
        if (IPO_BOOKMARKS_H > 0 && my >= IPO_TABBAR_H + IPO_TOOLBAR_H && my < IPO_TOP_CHROME) {
            int bmx = 8;
            for (int i = 0; i < s_bookmark_count; i++) {
                int lw = ui_text_w(s_bookmarks[i].label) + 24;
                if (bmx + lw > IPO_GUI_W - 8) break;
                if (mx >= bmx && mx < bmx + lw) { *out_cur = CURSOR_HAND; return 300 + i; }
                bmx += lw + 4;
            }
            return 0;
        }
        /* Status Bar Zoom Controls */
        if (my >= IPO_GUI_H - IPO_STATUSBAR_H) {
            statusbar_layout_t SL;
            compute_statusbar_layout(&SL);
            if (mx >= SL.zx     && mx < SL.zx + SL.btn_w)     { *out_cur = CURSOR_HAND; return 410; }
            if (mx >= SL.pct_x  && mx < SL.pct_x + SL.pct_w)  { *out_cur = CURSOR_HAND; return 411; }
            if (mx >= SL.plus_x && mx < SL.plus_x + SL.btn_w) { *out_cur = CURSOR_HAND; return 412; }
            return 0;
        }
    }

    /* Start Page or Video Player or Web Viewport hit tests */
    ipo_tab_t *tab = &s_tabs[s_active_tab];
    int vx = ipo_gui_vp_x(), vy = ipo_gui_vp_y();
    int vw = ipo_gui_vp_w(), vh = ipo_gui_vp_h();
    if (tab->used && tab->is_home) {
        start_layout_t L;
        compute_start_layout(vx, vy, vw, vh, &L);
        if (mx >= L.sx && mx < L.sx + L.sw && my >= L.sy && my < L.sy + L.sh) {
            if (mx >= L.sx + L.sw - L.go_w - 6) { *out_cur = CURSOR_HAND; return 900; }
            *out_cur = CURSOR_IBEAM;
            return 901;
        }
        for (int i = 0; i < 6; i++) {
            int cx = L.gx0 + (i % 3) * (L.cw + L.gap_x);
            int cy = L.gy0 + (i / 3) * (L.ch + L.gap_y);
            if (mx >= cx && mx < cx + L.cw && my >= cy && my < cy + L.ch) {
                *out_cur = CURSOR_HAND;
                return 910 + i;
            }
        }
        return 0;
    }

    if (tab->used && tab->is_video) {
        video_layout_t V;
        compute_video_layout(vx, vy, vw, vh, &V);
        if (mx >= V.back_x && mx < V.back_x + V.back_w &&
            my >= V.back_y && my < V.back_y + V.back_h) {
            *out_cur = CURSOR_HAND;
            return 810;
        }
        int sb_pad = V.compact ? 4 : 8;
        if (mx >= V.sb_x && mx < V.sb_x + V.sb_w &&
            my >= V.sb_y - sb_pad && my < V.sb_y + sb_pad + 6) {
            *out_cur = CURSOR_HAND;
            return 820; /* seekbar */
        }
        if (my >= V.btn_y && my < V.btn_y + V.btn_h) {
            if (mx >= V.play_x && mx < V.play_x + V.play_w) { *out_cur = CURSOR_HAND; return 811; }
            if (mx >= V.b5_x   && mx < V.b5_x + V.b5_w)     { *out_cur = CURSOR_HAND; return 812; }
            if (mx >= V.f5_x   && mx < V.f5_x + V.f5_w)     { *out_cur = CURSOR_HAND; return 813; }
            if (mx >= V.mute_x && mx < V.mute_x + V.mute_w) { *out_cur = CURSOR_HAND; return 814; }
            if (mx >= V.full_x && mx < V.full_x + V.full_w) { *out_cur = CURSOR_HAND; return 815; }
        }
        if (mx >= V.vid_x && mx < V.vid_x + V.vid_w && my >= V.vid_y && my < V.vid_y + V.vid_h) {
            *out_cur = CURSOR_HAND;
            return 821; /* canvas click = play/pause */
        }
        return 0;
    }

    /* Live Web Viewport: check scrollbar & hovered link/field for cursor shape */
    if (tab->used && tab->dev) {
        struct session *ses = dev_to_session(tab->dev);
        struct f_data_c *fd = ses ? current_frame(ses) : NULL;
        if (fd && fd->f_data) {
            if (fd->f_data->y > fd->yw && mx >= vx + vw - 12 && my >= vy && my < vy + vh) {
                *out_cur = CURSOR_HAND;
                return 950;
            }
            if (fd->vs && fd->vs->current_link >= 0 && fd->vs->current_link < fd->f_data->nlinks) {
                int ltype = fd->f_data->links[fd->vs->current_link].type;
                if (ltype == L_FIELD || ltype == L_AREA) *out_cur = CURSOR_IBEAM;
                else *out_cur = CURSOR_HAND;
            }
        }
    }

    return 0;
}

static void focus_omnibox_select_all(void) {
    s_focus = FOCUS_OMNIBOX;
    s_overlay = OVL_NONE;
    s_omnibox_sel_all = true;
    s_omnibox_cursor = s_omnibox_len;
    s_omnibox_scroll = 0;
    s_omnibox_last_click_ms = 0;
    s_dragging_textbox = 0;
    rebuild_omnibox_autocomplete();
    s_dirty_chrome = true;
    s_dirty_overlay = true;
}

static void textbox_handle_click(int drag_id, const char *buf, int len,
                                 int text_x, int max_w, int mx, bool was_focused,
                                 int *cursor, int *scroll_col, bool *sel_all,
                                 uint32_t *last_click_ms) {
    uint32_t now = gui_millis();
    if (was_focused && *last_click_ms != 0u && (uint32_t)(now - *last_click_ms) < 380u) {
        *sel_all = true;
        *cursor = len;
        *last_click_ms = 0;
        s_dragging_textbox = 0;
    } else {
        *sel_all = false;
        *cursor = textbox_cursor_from_x(buf, len, *scroll_col, text_x, mx);
        textbox_sync_scroll(buf, len, *cursor, scroll_col, max_w);
        *last_click_ms = now;
        s_dragging_textbox = drag_id;
    }
}

static void textbox_handle_drag(int mx) {
    if (!s_dragging_textbox || !s_tabs) return;
    int tx = 0, tw = 16;
    const char *buf = "";
    int len = 0, *cursor = NULL, *scroll_col = NULL;
    bool *sel_all = NULL;

    if (s_dragging_textbox == 1 && s_focus == FOCUS_OMNIBOX && !s_fullscreen) {
        toolbar_layout_t BL;
        int ty = 0;
        compute_toolbar_layout_ex(&BL);
        compute_omnibox_text_rect(&BL, &tx, &tw, &ty);
        buf = gui_str(s_omnibox_text); len = s_omnibox_len;
        cursor = &s_omnibox_cursor; scroll_col = &s_omnibox_scroll;
        sel_all = &s_omnibox_sel_all;
    } else if (s_dragging_textbox == 2 && s_focus == FOCUS_HERO && s_tabs[s_active_tab].is_home) {
        start_layout_t L;
        int hy = 0;
        compute_start_layout(ipo_gui_vp_x(), ipo_gui_vp_y(), ipo_gui_vp_w(), ipo_gui_vp_h(), &L);
        compute_hero_text_rect(&L, &tx, &tw, &hy);
        buf = gui_str(s_hero_text); len = s_hero_len;
        cursor = &s_hero_cursor; scroll_col = &s_hero_scroll;
        sel_all = &s_hero_sel_all;
    } else if (s_dragging_textbox == 3 && s_focus == FOCUS_FIND && s_overlay == OVL_FIND) {
        int fx, fy, fw, fh, param, in_x, in_w, prv_x, nxt_x, cls_x, nav_w, cls_w, btn_y, btn_h;
        compute_overlay_rect(OVL_FIND, &fx, &fy, &fw, &fh, &param);
        compute_find_layout(fx, fy, fw, fh, &in_x, &in_w, &prv_x, &nxt_x, &cls_x,
                            &nav_w, &cls_w, &btn_y, &btn_h);
        tx = in_x + 5; tw = in_w - 10;
        buf = gui_str(s_find_text); len = s_find_len;
        cursor = &s_find_cursor; scroll_col = &s_find_scroll;
        sel_all = &s_find_sel_all;
    } else {
        s_dragging_textbox = 0;
        return;
    }

    int prev_cur = *cursor, prev_sc = *scroll_col;
    if (mx < tx) {
        if (*scroll_col > 0) (*scroll_col)--;
        *cursor = utf8_col_to_byte(buf, len, *scroll_col);
    } else if (mx > tx + tw) {
        int total_cols = utf8_total_cols(buf, len);
        int right_col = *scroll_col;
        int avail_w = (tw > 14) ? (tw - 6) : 8;
        int w_acc = 0;
        const char *p = buf + utf8_col_to_byte(buf, len, *scroll_col);
        const char *end = buf + len;
        while (p < end) {
            uint32_t cp = utf8_next(&p);
            if (!cp) break;
            int adv = ui_glyph_adv(cp);
            if (w_acc + adv > avail_w) break;
            w_acc += adv;
            right_col++;
        }
        int target_col = (right_col < total_cols) ? (right_col + 1) : total_cols;
        *cursor = utf8_col_to_byte(buf, len, target_col);
    } else {
        *cursor = textbox_cursor_from_x(buf, len, *scroll_col, tx, mx);
    }
    *sel_all = false;
    textbox_sync_scroll(buf, len, *cursor, scroll_col, tw);
    if (*cursor != prev_cur || *scroll_col != prev_sc) {
        if (s_dragging_textbox == 1) s_dirty_chrome = true;
        else if (s_dragging_textbox == 2) s_dirty_vp = true;
        else if (s_dragging_textbox == 3) s_dirty_overlay = true;
    }
}

static void handle_ui_click(int hit, int mx, int my) {
    if (!s_tabs) return;
    ipo_tab_t *tab = &s_tabs[s_active_tab];

    /* Autocomplete item click */
    if (hit >= 0x100000 && hit < 0x100000 + s_ac_count) {
        int idx = hit - 0x100000;
        char *target = gui_fmt_alloc("%s", gui_str(s_ac_items[idx].url));
        s_focus = FOCUS_VIEWPORT;
        if (target) {
            navigate_tab_to(s_active_tab, target);
            free(target);
        }
        return;
    }

    if (hit == 0 && (s_overlay != OVL_NONE || s_focus == FOCUS_OMNIBOX)) {
        s_overlay = OVL_NONE;
        s_focus = FOCUS_VIEWPORT;
        trigger_tab_redraw(s_active_tab);
    }

    if (hit == 148) {
        if (s_tab_scroll > 0) { s_tab_scroll--; s_dirty_chrome = true; }
        return;
    }
    if (hit == 149) {
        int max_vis, vis_count, tw, f11_x, cls_x;
        compute_tabbar_layout(tab_count(), &max_vis, &vis_count, &tw, &f11_x, &cls_x);
        if (s_tab_scroll + max_vis < tab_count()) { s_tab_scroll++; s_dirty_chrome = true; }
        return;
    }
    if (hit >= 1000 && hit < 1000 + s_tabs_cap) {
        switch_to_tab(hit - 1000);
        return;
    }
    if (hit >= 2000 && hit < 2000 + s_tabs_cap) {
        close_tab(hit - 2000);
        return;
    }
    if (hit == 150) {
        open_new_tab("about:home");
        return;
    }
    if (hit == 160) {
        toggle_fullscreen();
        return;
    }
    if (hit == 161) {
        terminate_loop = 1;
        return;
    }

    if (hit == 200) { /* Back */
        if (tab->is_video) {
            video_stop_audio();
            tab->is_video = false;
            if (!tab->url || !tab->url[0] || yt_classify_url(tab->url) == YT_URL_VIDEO) {
                navigate_tab_to(s_active_tab, "about:home");
            } else {
                set_omnibox_str(tab->url);
                trigger_tab_redraw(s_active_tab);
            }
        } else if (!tab->is_home) {
            struct session *ses = dev_to_session(tab->dev);
            if (ses) {
                if (list_empty(ses->history) || ses->history.next == ses->history.prev) {
                    navigate_tab_to(s_active_tab, "about:home");
                } else {
                    go_back(ses, 1);
                }
            }
        }
        return;
    }
    if (hit == 201) { /* Forward */
        struct session *ses = dev_to_session(tab->dev);
        if (ses && !tab->is_home && !tab->is_video) go_back(ses, -1);
        return;
    }
    if (hit == 202) { /* Reload / Stop */
        if (is_tab_loading(s_active_tab)) {
            stop_tab_loading(s_active_tab);
        } else if (tab->is_home) {
            s_dirty_vp = true;
        } else if (tab->is_video) {
            s_vid_pos_ms = 0;
            if (s_mp4_player) mp4_player_seek_ms(s_mp4_player, 0u);
            s_dirty_vp = true;
        } else {
            struct session *ses = dev_to_session(tab->dev);
            if (ses) reload(ses, -1);
        }
        return;
    }
    if (hit == 203) { /* Home */
        navigate_tab_to(s_active_tab, "about:home");
        return;
    }
    if (hit == 208) { /* Security / Page Info badge */
        overlay_kind_t next = (s_overlay == OVL_SECURITY) ? OVL_NONE : OVL_SECURITY;
        s_overlay = next;
        s_focus = FOCUS_VIEWPORT;
        if (next == OVL_NONE) trigger_tab_redraw(s_active_tab);
        else { s_dirty_chrome = true; s_dirty_overlay = true; }
        return;
    }
    if (hit == 209) { /* Omnibox click */
        bool was_focused = (s_focus == FOCUS_OMNIBOX);
        toolbar_layout_t BL;
        int otxt_x, otxt_w, otxt_y;
        compute_toolbar_layout_ex(&BL);
        compute_omnibox_text_rect(&BL, &otxt_x, &otxt_w, &otxt_y);
        s_focus = FOCUS_OMNIBOX;
        s_overlay = OVL_NONE;
        textbox_handle_click(1, gui_str(s_omnibox_text), s_omnibox_len, otxt_x, otxt_w, mx,
                             was_focused, &s_omnibox_cursor, &s_omnibox_scroll,
                             &s_omnibox_sel_all, &s_omnibox_last_click_ms);
        rebuild_omnibox_autocomplete();
        s_dirty_chrome = true;
        s_dirty_overlay = true;
        return;
    }
    if (hit == 210) { /* Bookmark star */
        toggle_bookmark_current();
        return;
    }
    if (hit == 220) { /* Downloads toggle */
        overlay_kind_t next = (s_overlay == OVL_DOWNLOADS) ? OVL_NONE : OVL_DOWNLOADS;
        s_overlay = next;
        if (next == OVL_NONE) trigger_tab_redraw(s_active_tab);
        else { trigger_tab_redraw(s_active_tab); s_overlay = next; s_dirty_chrome = true; s_dirty_overlay = true; }
        return;
    }
    if (hit == 224) { /* History toggle */
        overlay_kind_t next = (s_overlay == OVL_HISTORY) ? OVL_NONE : OVL_HISTORY;
        s_overlay = next;
        if (next == OVL_NONE) trigger_tab_redraw(s_active_tab);
        else { trigger_tab_redraw(s_active_tab); s_overlay = next; s_dirty_chrome = true; s_dirty_overlay = true; }
        return;
    }
    if (hit == 221) { /* Find toggle */
        overlay_kind_t next = (s_overlay == OVL_FIND) ? OVL_NONE : OVL_FIND;
        s_overlay = next;
        if (next == OVL_NONE) { s_focus = FOCUS_VIEWPORT; trigger_tab_redraw(s_active_tab); }
        else { trigger_tab_redraw(s_active_tab); s_overlay = next; s_focus = FOCUS_FIND; s_dirty_chrome = true; s_dirty_overlay = true; }
        return;
    }
    if (hit == 222) { /* YT Player toggle */
        if (tab->is_video) {
            handle_ui_click(200, mx, my);
        } else {
            enter_video_in_tab(s_active_tab, "https://www.youtube.com/watch?v=YE7VzlLtp-4");
        }
        return;
    }
    if (hit == 223) { /* Menu toggle */
        overlay_kind_t next = (s_overlay == OVL_MENU) ? OVL_NONE : OVL_MENU;
        s_overlay = next;
        if (next == OVL_NONE) trigger_tab_redraw(s_active_tab);
        else { trigger_tab_redraw(s_active_tab); s_overlay = next; s_dirty_chrome = true; s_dirty_overlay = true; }
        return;
    }

    if (hit >= 300 && hit < 300 + s_bookmark_count) {
        navigate_tab_to(s_active_tab, gui_str(s_bookmarks[hit - 300].url));
        return;
    }

    /* Zoom buttons (Status bar & Menu) */
    if (hit == 410 || hit == 720) { apply_zoom_delta(-15); return; }
    if (hit == 411 || hit == 721) { apply_zoom_delta(0);   return; }
    if (hit == 412 || hit == 722) { apply_zoom_delta(+15); return; }

    /* Downloads / Security / History drawer buttons */
    if (hit == 500 || hit == 520 || hit == 559) {
        s_overlay = OVL_NONE;
        trigger_tab_redraw(s_active_tab);
        return;
    }
    if (hit == 501) {
        const char *u = (tab->url && tab->url[0] && !tab->is_home) ? tab->url : "https://example.com";
        gui_start_download(u);
        return;
    }
    if (hit == 502) { gui_start_download("https://example.com"); return; }
    if (hit == 503) { gui_start_download("https://www.google.com"); return; }

    if (hit >= 0x200000 && hit < 0x200000 + s_history_count) {
        int h_idx = s_history_count - 1 - (hit - 0x200000);
        if (h_idx >= 0 && h_idx < s_history_count) {
            char *target = gui_fmt_alloc("%s", gui_str(s_history[h_idx].url));
            s_overlay = OVL_NONE;
            if (target) {
                navigate_tab_to(s_active_tab, target);
                free(target);
            }
        }
        return;
    }
    if (hit == 560) {
        for (int i = 0; i < s_history_count; i++) {
            free(s_history[i].title);
            free(s_history[i].url);
        }
        s_history_count = 0;
        gui_set_str(&s_status_msg, "Browsing history cleared");
        s_dirty_chrome = true;
        s_dirty_overlay = true;
        return;
    }

    /* Find bar */
    if (hit == 600) {
        bool was_focused = (s_focus == FOCUS_FIND);
        int fx, fy, fw, fh, param, in_x, in_w, prv_x, nxt_x, cls_x, nav_w, cls_w, btn_y, btn_h;
        compute_overlay_rect(OVL_FIND, &fx, &fy, &fw, &fh, &param);
        compute_find_layout(fx, fy, fw, fh, &in_x, &in_w, &prv_x, &nxt_x, &cls_x,
                            &nav_w, &cls_w, &btn_y, &btn_h);
        s_focus = FOCUS_FIND;
        textbox_handle_click(3, gui_str(s_find_text), s_find_len, in_x + 5, in_w - 10, mx,
                             was_focused, &s_find_cursor, &s_find_scroll,
                             &s_find_sel_all, &s_find_last_click_ms);
        s_dirty_overlay = true;
        return;
    }
    if (hit == 601) { execute_find(true); return; }
    if (hit == 602) { execute_find(false); return; }
    if (hit == 603) { s_overlay = OVL_NONE; s_focus = FOCUS_VIEWPORT; trigger_tab_redraw(s_active_tab); return; }

    /* Menu items */
    if (hit == 700) { s_overlay = OVL_NONE; open_new_tab("about:home"); return; }
    if (hit == 701) { s_overlay = OVL_NONE; close_tab(s_active_tab); return; }
    if (hit == 702) { trigger_tab_redraw(s_active_tab); s_overlay = OVL_DOWNLOADS; s_dirty_chrome = true; s_dirty_overlay = true; return; }
    if (hit == 708) { trigger_tab_redraw(s_active_tab); s_overlay = OVL_HISTORY;   s_dirty_chrome = true; s_dirty_overlay = true; return; }
    if (hit == 703) { trigger_tab_redraw(s_active_tab); s_overlay = OVL_FIND; s_focus = FOCUS_FIND; s_dirty_chrome = true; s_dirty_overlay = true; return; }
    if (hit == 704) { s_overlay = OVL_NONE; toggle_bookmark_current(); trigger_tab_redraw(s_active_tab); return; }
    if (hit == 705) { toggle_fullscreen(); return; }
    if (hit == 706) {
        s_overlay = OVL_NONE;
        tab->is_home = false; tab->is_video = false;
        trigger_tab_redraw(s_active_tab);
        struct session *ses = dev_to_session(tab->dev);
        if (ses) activate_bfu_technology(ses, -1);
        return;
    }
    if (hit == 707) { terminate_loop = 1; return; }

    /* Right-Click Context Menu items */
    if (hit == 750) { /* Open Link in New Tab */
        s_overlay = OVL_NONE;
        if (s_ctx_link_url && s_ctx_link_url[0]) open_new_tab(s_ctx_link_url);
        return;
    }
    if (hit == 751) { /* Copy Link Address */
        s_overlay = OVL_NONE;
        if (s_ctx_link_url && s_ctx_link_url[0]) {
            set_clipboard_text(NULL, (unsigned char *)s_ctx_link_url);
            gui_set_fmt(&s_status_msg, "Copied link: %s", s_ctx_link_url);
        }
        trigger_tab_redraw(s_active_tab);
        return;
    }
    if (hit == 752) { /* Download Link Target */
        s_overlay = OVL_NONE;
        if (s_ctx_link_url && s_ctx_link_url[0]) gui_start_download(s_ctx_link_url);
        return;
    }
    if (hit == 753) { s_overlay = OVL_NONE; trigger_tab_redraw(s_active_tab); handle_ui_click(200, mx, my); return; }
    if (hit == 754) { s_overlay = OVL_NONE; trigger_tab_redraw(s_active_tab); handle_ui_click(201, mx, my); return; }
    if (hit == 755) { s_overlay = OVL_NONE; trigger_tab_redraw(s_active_tab); handle_ui_click(202, mx, my); return; }
    if (hit == 756) { s_overlay = OVL_NONE; toggle_bookmark_current(); trigger_tab_redraw(s_active_tab); return; }
    if (hit == 757) {
        s_overlay = OVL_NONE;
        const char *u = (tab->url && tab->url[0] && !tab->is_home) ? tab->url : "https://example.com";
        gui_start_download(u);
        return;
    }
    if (hit == 758) { handle_ui_click(703, mx, my); return; }
    if (hit == 759) { trigger_tab_redraw(s_active_tab); s_overlay = OVL_SECURITY; s_dirty_chrome = true; s_dirty_overlay = true; return; }

    /* Viewport Vertical Scrollbar click/drag */
    if (hit == 950) {
        s_dragging_vscroll = true;
        scroll_viewport_to_mouse_y(my);
        return;
    }

    /* Video player controls */
    if (hit == 810) { handle_ui_click(200, mx, my); return; }
    if (hit == 811 || hit == 821) {
        s_vid_paused = !s_vid_paused;
        if (s_mp4_player) mp4_player_set_paused(s_mp4_player, s_vid_paused);
        s_dirty_vp = true;
        return;
    }
    if (hit == 812) {
        uint32_t cur = s_mp4_player ? mp4_player_pos_ms(s_mp4_player) : s_vid_pos_ms;
        s_vid_pos_ms = (cur > 5000u) ? (cur - 5000u) : 0u;
        if (s_mp4_player) mp4_player_seek_ms(s_mp4_player, s_vid_pos_ms);
        s_dirty_vp = true;
        return;
    }
    if (hit == 813) {
        uint32_t cur = s_mp4_player ? mp4_player_pos_ms(s_mp4_player) : s_vid_pos_ms;
        uint32_t dur = s_mp4_player ? mp4_player_duration_ms(s_mp4_player) : 60000u;
        s_vid_pos_ms = (cur + 5000u < dur) ? (cur + 5000u) : (dur > 1000u ? dur - 1000u : 0u);
        if (s_mp4_player) mp4_player_seek_ms(s_mp4_player, s_vid_pos_ms);
        s_dirty_vp = true;
        return;
    }
    if (hit == 814) {
        s_vid_muted = !s_vid_muted;
        if (s_mp4_player) mp4_player_set_muted(s_mp4_player, s_vid_muted);
        s_dirty_vp = true;
        return;
    }
    if (hit == 815) { toggle_fullscreen(); return; }
    if (hit == 820) {
        video_layout_t V;
        compute_video_layout(ipo_gui_vp_x(), ipo_gui_vp_y(), ipo_gui_vp_w(), ipo_gui_vp_h(), &V);
        int rx = mx - V.sb_x;
        if (rx < 0) rx = 0;
        if (rx > V.sb_w) rx = V.sb_w;
        uint32_t dur = s_mp4_player ? mp4_player_duration_ms(s_mp4_player) : 60000u;
        s_vid_pos_ms = (V.sb_w > 0) ? (uint32_t)(((uint64_t)rx * (uint64_t)dur) / (uint64_t)V.sb_w) : 0u;
        s_vid_dragging_seek = 1;
        s_dirty_vp = true;
        return;
    }

    /* Start Page */
    if (hit == 900) {
        if (s_hero_len > 0) navigate_tab_to(s_active_tab, gui_str(s_hero_text));
        else { s_focus = FOCUS_HERO; s_dirty_vp = true; }
        return;
    }
    if (hit == 901) {
        bool was_focused = (s_focus == FOCUS_HERO);
        start_layout_t L;
        int hx, hw, hy;
        compute_start_layout(ipo_gui_vp_x(), ipo_gui_vp_y(), ipo_gui_vp_w(), ipo_gui_vp_h(), &L);
        compute_hero_text_rect(&L, &hx, &hw, &hy);
        s_focus = FOCUS_HERO;
        textbox_handle_click(2, gui_str(s_hero_text), s_hero_len, hx, hw, mx,
                             was_focused, &s_hero_cursor, &s_hero_scroll,
                             &s_hero_sel_all, &s_hero_last_click_ms);
        s_dirty_vp = true;
        return;
    }
    if (hit >= 910 && hit < 916) {
        int ci = hit - 910;
        static const char *card_urls[6] = {
            "https://www.google.com",
            "https://www.youtube.com",
            "https://www.youtube.com/watch?v=YE7VzlLtp-4",
            "https://en.wikipedia.org",
            "https://example.com",
            NULL
        };
        if (ci == 5) {
            s_overlay = OVL_DOWNLOADS;
            s_dirty_chrome = true;
            s_dirty_overlay = true;
        } else {
            navigate_tab_to(s_active_tab, card_urls[ci]);
        }
        return;
    }
}

/* =========================================================================
 * Text Box & Keyboard Dispatcher (Set 1 + 0xE0 + UTF-8 Cyrillic)
 * ========================================================================= */
static void utf8_pop_prev_char(char *buf, int *len, int *cursor) {
    if (*cursor <= 0) return;
    int start = *cursor - 1;
    while (start > 0 && ((unsigned char)buf[start] & 0xC0u) == 0x80u) start--;
    int del_len = *cursor - start;
    memmove(buf + start, buf + *cursor, (size_t)(*len - *cursor + 1));
    *len -= del_len;
    *cursor = start;
}

static void text_edit_insert(char **pbuf, size_t *pcap, int *len, int *cursor, bool *sel_all, const char *utf8) {
    if (!pbuf || !pcap || !len || !cursor || !utf8) return;
    if (!gui_dyn_ensure(pbuf, pcap, 1u)) return;
    if (sel_all && *sel_all) {
        (*pbuf)[0] = '\0';
        *len = 0;
        *cursor = 0;
        *sel_all = false;
    }
    size_t ins_sz = strlen(utf8);
    if (ins_sz > (size_t)0x7fffffff || (size_t)*len > (size_t)0x7ffffffe - ins_sz) return;
    int ins = (int)ins_sz;
    if (!gui_dyn_ensure(pbuf, pcap, (size_t)*len + ins_sz + 1u)) return;
    char *buf = *pbuf;
    memmove(buf + *cursor + ins, buf + *cursor, (size_t)(*len - *cursor + 1));
    memcpy(buf + *cursor, utf8, ins_sz);
    *len += ins;
    *cursor += ins;
}

static void scancode_to_utf8(uint8_t sc, bool shift, char out[8]) {
    out[0] = '\0';
    /* Set 1 US ASCII table indexed by scancode so rapid Shift bursts stay synced to FIFO order. */
    static const char unshifted[0x3A] = {
        /* 0x00 */ 0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
        /* 0x10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0, 'a', 's',
        /* 0x20 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
        /* 0x30 */ 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };
    static const char shifted[0x3A] = {
        /* 0x00 */ 0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
        /* 0x10 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0, 'A', 'S',
        /* 0x20 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
        /* 0x30 */ 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
    };
    const char *probe = keyboard_get_key_string(0x1E);
    bool is_us = (!probe || probe[0] == 'a' || probe[0] == 'A');
    if (is_us && sc < 0x3A) {
        char c = shift ? shifted[sc] : unshifted[sc];
        if (s_mod_caps && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        else if (s_mod_caps && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c) {
            out[0] = c;
            out[1] = '\0';
            return;
        }
    }
    const char *ks = keyboard_get_key_string(sc);
    if (ks && ks[0] && (unsigned char)ks[0] >= 0x20u && (unsigned char)ks[0] != 0x7Fu) {
        snprintf(out, 8, "%s", ks);
    }
}

static bool handle_textbox_key(char **pbuf, size_t *pcap, int *len, int *cursor, bool *sel_all,
                               uint8_t sc, bool ext, bool ctrl, bool shift, bool *out_enter) {
    *out_enter = false;
    if (sc == 0x1C) { *out_enter = true; return true; }
    if (!pbuf || !pcap || !gui_dyn_ensure(pbuf, pcap, 1u)) return false;
    char *buf = *pbuf;
    if (sc == 0x0E) { /* Backspace / Ctrl+Backspace */
        if (sel_all && *sel_all) {
            buf[0] = '\0'; *len = 0; *cursor = 0; *sel_all = false;
        } else if (ctrl) {
            int orig = *cursor;
            utf8_jump_word_left(buf, cursor);
            if (*cursor < orig) {
                memmove(buf + *cursor, buf + orig, (size_t)(*len - orig + 1));
                *len -= (orig - *cursor);
            }
        } else {
            utf8_pop_prev_char(buf, len, cursor);
        }
        return true;
    }
    if (ext && sc == 0x53) { /* Delete */
        if (sel_all && *sel_all) {
            buf[0] = '\0'; *len = 0; *cursor = 0; *sel_all = false;
        } else if (ctrl && *cursor < *len) {
            int end = *cursor;
            utf8_jump_word_right(buf, *len, &end);
            if (end > *cursor) {
                memmove(buf + *cursor, buf + end, (size_t)(*len - end + 1));
                *len -= (end - *cursor);
            }
        } else if (*cursor < *len) {
            int end = *cursor + 1;
            while (end < *len && ((unsigned char)buf[end] & 0xC0u) == 0x80u) end++;
            memmove(buf + *cursor, buf + end, (size_t)(*len - end + 1));
            *len -= (end - *cursor);
        }
        return true;
    }
    if (ext && sc == 0x4B) { /* Left / Ctrl+Left */
        if (sel_all) *sel_all = false;
        if (ctrl) {
            utf8_jump_word_left(buf, cursor);
        } else if (*cursor > 0) {
            (*cursor)--;
            while (*cursor > 0 && ((unsigned char)buf[*cursor] & 0xC0u) == 0x80u) (*cursor)--;
        }
        return true;
    }
    if (ext && sc == 0x4D) { /* Right / Ctrl+Right */
        if (sel_all) *sel_all = false;
        if (ctrl) {
            utf8_jump_word_right(buf, *len, cursor);
        } else if (*cursor < *len) {
            (*cursor)++;
            while (*cursor < *len && ((unsigned char)buf[*cursor] & 0xC0u) == 0x80u) (*cursor)++;
        }
        return true;
    }
    if (ext && sc == 0x47) { if (sel_all) *sel_all = false; *cursor = 0; return true; }
    if (ext && sc == 0x4F) { if (sel_all) *sel_all = false; *cursor = *len; return true; }
    if (ctrl && sc == 0x1E) { if (sel_all) *sel_all = true; *cursor = *len; return true; } /* Ctrl+A */
    if (ctrl && sc == 0x16) { buf[0] = '\0'; *len = 0; *cursor = 0; if (sel_all) *sel_all = false; return true; } /* Ctrl+U */
    if (ctrl && sc == 0x2E) { /* Ctrl+C */
        set_clipboard_text(NULL, (unsigned char *)buf);
        return true;
    }
    if (ctrl && sc == 0x2D) { /* Ctrl+X */
        set_clipboard_text(NULL, (unsigned char *)buf);
        buf[0] = '\0'; *len = 0; *cursor = 0;
        if (sel_all) *sel_all = false;
        return true;
    }
    if (ctrl && sc == 0x2F) { /* Ctrl+V */
        unsigned char *clip = get_clipboard_text(NULL);
        if (clip) {
            text_edit_insert(pbuf, pcap, len, cursor, sel_all, (const char *)clip);
            mem_free(clip);
        }
        return true;
    }
    if (!ctrl && !ext) {
        char ks[8];
        scancode_to_utf8(sc, shift, ks);
        if (ks[0]) {
            text_edit_insert(pbuf, pcap, len, cursor, sel_all, ks);
            return true;
        }
    }
    return false;
}

static void dispatch_scancode(uint8_t raw_sc) {
    if (raw_sc == 0) return;
    if (raw_sc == 0xE0) { s_scancode_e0 = true; return; }
    bool ext = s_scancode_e0;
    s_scancode_e0 = false;

    bool released = (raw_sc & 0x80u) != 0;
    uint8_t sc = raw_sc & 0x7Fu;

    if (sc == 0x1D) { s_mod_ctrl = !released; return; }
    if (sc == 0x2A || sc == 0x36) { s_mod_shift = !released; return; }
    if (sc == 0x38) { s_mod_alt = !released; return; }
    if (sc == 0x3A && !ext) { if (!released) s_mod_caps = !s_mod_caps; return; }
    if (released) return;

    uint8_t hw_mods = keyboard_get_modifiers();
    bool ctrl  = s_mod_ctrl  || ((hw_mods & KBD_MOD_CTRL)  != 0);
    bool shift = s_mod_shift || ((hw_mods & KBD_MOD_SHIFT) != 0);
    bool alt   = s_mod_alt   || ((hw_mods & KBD_MOD_ALT)   != 0);

    if (!s_tabs || s_active_tab < 0 || s_active_tab >= s_tabs_cap) return;
    ipo_tab_t *tab = &s_tabs[s_active_tab];

    /* Global Browser Shortcuts */
    if (sc == 0x57 && !ext) { toggle_fullscreen(); return; } /* F11 */
    if (sc == 0x44 && !ext) { handle_ui_click(706, 0, 0); return; } /* F10 -> Links Menu */
    if (sc == 0x40 && !ext) { focus_omnibox_select_all(); return; } /* F6 -> Omnibox */
    if (sc == 0x3F && !ext) { handle_ui_click(202, 0, 0); return; } /* F5 -> Reload */
    if (ctrl && sc == 0x26) { focus_omnibox_select_all(); return; } /* Ctrl+L */
    if (alt  && sc == 0x20) { focus_omnibox_select_all(); return; } /* Alt+D */
    if (ctrl && sc == 0x14) { open_new_tab("about:home"); return; } /* Ctrl+T */
    if (ctrl && sc == 0x11) { close_tab(s_active_tab); return; }    /* Ctrl+W */
    if (ctrl && sc == 0x10) { terminate_loop = 1; return; }         /* Ctrl+Q */
    if (alt  && sc == 0x3E) { terminate_loop = 1; return; }         /* Alt+F4 */
    if (ctrl && sc == 0x24) { handle_ui_click(220, 0, 0); return; } /* Ctrl+J -> Downloads */
    if (ctrl && sc == 0x23) { handle_ui_click(224, 0, 0); return; } /* Ctrl+H -> History */
    if (ctrl && sc == 0x1F) { handle_ui_click(501, 0, 0); return; } /* Ctrl+S -> Save/Download Page */
    if (ctrl && sc == 0x21) { handle_ui_click(221, 0, 0); return; } /* Ctrl+F -> Find in Page */
    if (ctrl && sc == 0x20) { toggle_bookmark_current(); return; }  /* Ctrl+D -> Bookmark */
    if (ctrl && sc == 0x13) { handle_ui_click(202, 0, 0); return; } /* Ctrl+R -> Reload */
    if (ctrl && sc == 0x0D) { apply_zoom_delta(+15); return; }      /* Ctrl++ / Ctrl+= -> Zoom In */
    if (ctrl && sc == 0x0C) { apply_zoom_delta(-15); return; }      /* Ctrl+- -> Zoom Out */
    if (ctrl && sc == 0x0B) { apply_zoom_delta(0); return; }        /* Ctrl+0 -> Reset Zoom */
    if (alt  && ext && sc == 0x4B) { handle_ui_click(200, 0, 0); return; } /* Alt+Left */
    if (alt  && ext && sc == 0x4D) { handle_ui_click(201, 0, 0); return; } /* Alt+Right */
    if (ctrl && sc == 0x0F) { /* Ctrl+Tab / Ctrl+Shift+Tab */
        int n = s_tabs_cap;
        if (n <= 0) return;
        int dir = shift ? -1 : 1;
        for (int step = 1; step <= n; step++) {
            int cand = (s_active_tab + dir * step + n) % n;
            if (s_tabs[cand].used) { switch_to_tab(cand); break; }
        }
        return;
    }

    /* Esc cancels overlays/focus/fullscreen/loading */
    if (sc == 0x01) {
        if (s_overlay != OVL_NONE) {
            s_overlay = OVL_NONE;
            s_focus = FOCUS_VIEWPORT;
            trigger_tab_redraw(s_active_tab);
            return;
        }
        if (s_focus != FOCUS_VIEWPORT) {
            s_focus = FOCUS_VIEWPORT;
            s_omnibox_sel_all = false;
            ac_clear_items();
            s_ac_sel = -1;
            s_dirty_chrome = true;
            if (tab->is_home) s_dirty_vp = true;
            return;
        }
        if (is_tab_loading(s_active_tab)) {
            stop_tab_loading(s_active_tab);
            return;
        }
        if (s_fullscreen) { toggle_fullscreen(); return; }
    }

    /* Active Text Box Focus Handling */
    if (s_focus == FOCUS_OMNIBOX) {
        if (ext && sc == 0x50 && s_ac_count > 0) { /* Down arrow in autocomplete */
            s_ac_sel = (s_ac_sel + 1) % s_ac_count;
            set_omnibox_str(gui_str(s_ac_items[s_ac_sel].url));
            s_omnibox_sel_all = false;
            s_dirty_chrome = true;
            s_dirty_vp = true;
            return;
        }
        if (ext && sc == 0x48 && s_ac_count > 0) { /* Up arrow in autocomplete */
            s_ac_sel = (s_ac_sel <= 0) ? (s_ac_count - 1) : (s_ac_sel - 1);
            set_omnibox_str(gui_str(s_ac_items[s_ac_sel].url));
            s_omnibox_sel_all = false;
            s_dirty_chrome = true;
            s_dirty_vp = true;
            return;
        }
        bool hit_enter = false;
        if (handle_textbox_key(&s_omnibox_text, &s_omnibox_cap,
                               &s_omnibox_len, &s_omnibox_cursor, &s_omnibox_sel_all,
                               sc, ext, ctrl, shift, &hit_enter)) {
            if (hit_enter) {
                const char *target = (s_ac_sel >= 0 && s_ac_sel < s_ac_count && s_ac_items[s_ac_sel].url)
                                     ? s_ac_items[s_ac_sel].url : gui_str(s_omnibox_text);
                char *target_copy = gui_fmt_alloc("%s", target);
                s_ac_sel = -1;
                if (target_copy) {
                    navigate_tab_to(s_active_tab, target_copy);
                    free(target_copy);
                }
            } else {
                rebuild_omnibox_autocomplete();
                s_dirty_chrome = true;
                s_dirty_vp = true;
            }
            return;
        }
    } else if (s_focus == FOCUS_HERO && tab->is_home) {
        bool hit_enter = false;
        if (handle_textbox_key(&s_hero_text, &s_hero_cap,
                               &s_hero_len, &s_hero_cursor, &s_hero_sel_all,
                               sc, ext, ctrl, shift, &hit_enter)) {
            if (hit_enter) navigate_tab_to(s_active_tab, gui_str(s_hero_text));
            else s_dirty_vp = true;
            return;
        }
    } else if (s_focus == FOCUS_FIND && s_overlay == OVL_FIND) {
        bool hit_enter = false;
        if (handle_textbox_key(&s_find_text, &s_find_cap,
                               &s_find_len, &s_find_cursor, &s_find_sel_all,
                               sc, ext, ctrl, shift, &hit_enter)) {
            if (hit_enter) execute_find(shift);
            else s_dirty_overlay = true;
            return;
        }
    }

    /* Embedded Video Player Shortcuts */
    if (tab->used && tab->is_video) {
        if (sc == 0x39 || sc == 0x25) { handle_ui_click(811, 0, 0); return; } /* Space / K */
        if ((ext && sc == 0x4B) || sc == 0x24) { handle_ui_click(812, 0, 0); return; } /* Left / J */
        if ((ext && sc == 0x4D) || sc == 0x26) { handle_ui_click(813, 0, 0); return; } /* Right / L */
        if (sc == 0x32) { handle_ui_click(814, 0, 0); return; } /* M Mute */
        return;
    }

    /* Forward to Live Links Browser Viewport */
    if (!tab->used || !tab->dev || !tab->dev->keyboard_handler || tab->is_home) return;
    int kflags = (shift ? KBD_SHIFT : 0) | (ctrl ? KBD_CTRL : 0) | (alt ? KBD_ALT : 0);
    int kkey = 0;
    if (ext) {
        switch (sc) {
            case 0x48: kkey = KBD_UP; break;
            case 0x50: kkey = KBD_DOWN; break;
            case 0x4B: kkey = KBD_LEFT; break;
            case 0x4D: kkey = KBD_RIGHT; break;
            case 0x49: kkey = KBD_PAGE_UP; break;
            case 0x51: kkey = KBD_PAGE_DOWN; break;
            case 0x47: kkey = KBD_HOME; break;
            case 0x4F: kkey = KBD_END; break;
            case 0x52: kkey = KBD_INS; break;
            case 0x53: kkey = KBD_DEL; break;
            default: break;
        }
    } else {
        switch (sc) {
            case 0x1C: kkey = KBD_ENTER; break;
            case 0x0E: kkey = KBD_BS; break;
            case 0x0F: kkey = KBD_TAB; break;
            case 0x01: kkey = KBD_ESC; break;
            case 0x3B: kkey = KBD_F1; break;
            case 0x3C: kkey = KBD_F2; break;
            case 0x3D: kkey = KBD_F3; break;
            case 0x3E: kkey = KBD_F4; break;
            case 0x39: kkey = ' '; break;
            default: {
                if (ctrl) {
                    static const char cletters[] = {
                        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                        'q','w','e','r','t','y','u','i','o','p',0,0,0,0,
                        'a','s','d','f','g','h','j','k','l',0,0,0,0,0,
                        'z','x','c','v','b','n','m'
                    };
                    if (sc < sizeof(cletters) && cletters[sc]) kkey = cletters[sc];
                } else {
                    char ks[8];
                    scancode_to_utf8(sc, shift, ks);
                    if (ks[0]) {
                        const char *p = ks;
                        kkey = (int)utf8_next(&p);
                    }
                }
                break;
            }
        }
    }
    if (kkey != 0) {
        tab->dev->keyboard_handler(tab->dev, kkey, kflags);
    }
}

/* =========================================================================
 * Public ipo_gui API
 * ========================================================================= */
int ipo_gui_consume_wakeup(void) {
    if (!s_links_wakeup) return 0;
    s_links_wakeup = false;
    return 1;
}

void ipo_gui_init(void) {
    if (s_inited) return;
    s_shutting_down = false;
    s_fullscreen = false;
    s_direct_gfx = false;
    s_wm_win = NULL;

    vga_gfx_probe_screen_size();
    int sw = 320, sh = 200;
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320 || sw > 4096) sw = 320;
    if (sh < 200 || sh > 4096) sh = 200;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Links";
    opt.decor_style = WM_DECOR_DEFAULT;
    opt.bg_color = 0;
    opt.w = (sw > 800) ? (uint16_t)(sw - 120) : (uint16_t)(sw > 16 ? sw - 12 : 300);
    opt.h = (sh > 500) ? (uint16_t)(sh - 80) : (uint16_t)(sh > 36 ? sh - 32 : 168);
    opt.x = (int16_t)((sw > (int)opt.w) ? (sw - (int)opt.w) / 2 : 0);
    opt.y = (int16_t)((sh > (int)opt.h + 26) ? (sh - (int)opt.h - 20) / 2 : 10);

    s_hw_direct_fb = false;
    s_force_hw_reflip = false;
    {
        ipo_gfx_caps_t gcaps;
        s_use_truecolor32 = (ipo_gfx_get_caps(&gcaps) == 0 && gcaps.active_hw_bpp >= 15u);
    }
    s_wm_win = wm_create_window(&opt);
    if (s_wm_win) {
        wm_set_fullscreen(s_wm_win, true);
        wm_set_cursor_visible(s_wm_win, s_use_truecolor32 ? true : false);
        vga_gfx_probe_screen_size();
        vga_gfx_get_screen_bounds(&sw, &sh);
        if (sw < 320 || sw > 4096) sw = 320;
        if (sh < 200 || sh > 4096) sh = 200;
        if (s_use_truecolor32) {
            s_hw_direct_fb = false;
            s_gui_w = sw;
            s_gui_h = sh;
        } else if (sw > (int)s_wm_win->w || sh > (int)s_wm_win->h) {
            s_hw_direct_fb = true;
            s_gui_w = sw;
            s_gui_h = sh;
        } else {
            s_gui_w = (int)s_wm_win->w;
            s_gui_h = (int)s_wm_win->h;
        }
    } else {
        vga_set_mode_13h();
        vga_hide_overlay_cursor();
        vga_show_close_button();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        s_direct_gfx = true;
        vga_gfx_get_screen_bounds(&sw, &sh);
        s_gui_w = sw;
        s_gui_h = sh;
        {
            ipo_gfx_caps_t gcaps;
            s_use_truecolor32 = (ipo_gfx_get_caps(&gcaps) == 0 && gcaps.active_hw_bpp >= 15u);
        }
    }
    if (s_gui_w < 320 || s_gui_w > 4096) s_gui_w = 320;
    if (s_gui_h < 200 || s_gui_h > 4096) s_gui_h = 200;
    s_wm_last_w = s_wm_win ? (int)s_wm_win->w : s_gui_w;
    s_wm_last_h = s_wm_win ? (int)s_wm_win->h : s_gui_h;
    s_wm_cur_w = 0;
    s_wm_cur_h = 0;

    s_fb32 = (uint32_t *)calloc((size_t)s_gui_w * (size_t)s_gui_h, sizeof(uint32_t));
    s_fb8_base = (uint8_t *)calloc((size_t)s_gui_w * (size_t)s_gui_h, 1);
    s_lut_near = (uint8_t *)malloc((size_t)1u << 15u);
    s_tabs = NULL;
    s_tabs_cap = 0;
    s_tab_scroll = 0;
    ensure_tab_slot();
    if (s_dls && s_dls_cap > 0) memset(s_dls, 0, (size_t)s_dls_cap * sizeof(dl_slot_t));
    s_active_tab = 0;
    s_overlay = OVL_NONE;
    s_focus = FOCUS_VIEWPORT;
    s_zoom_pct = 100;
    s_ac_count = 0;
    s_ac_sel = -1;
    init_default_bookmarks();

    init_palette_and_lut();

    mouse_state_t ms;
    mouse_get_state(&ms);
    int init_mx, init_my;
    bool use_rel_hd = (s_hw_direct_fb && s_wm_win) ||
                      (s_direct_gfx && (s_gui_w > 320 || s_gui_h > 200));
    if (use_rel_hd) {
        mouse_get_cumulative_motion(&s_prev_cm_x, &s_prev_cm_y);
        s_prev_wm_mx = ms.x;
        s_prev_wm_my = ms.y;
        if (s_wm_win) {
            int cw = (s_wm_win->w > 1) ? (int)s_wm_win->w : 1;
            int ch = (s_wm_win->h > 1) ? (int)s_wm_win->h : 1;
            int rx = ms.x - s_wm_win->x;
            int ry = ms.y - s_wm_win->y;
            init_mx = (rx * s_gui_w) / cw;
            init_my = (ry * s_gui_h) / ch;
            if (s_wm_win->framebuf) {
                memset(s_wm_win->framebuf, 0, (size_t)s_wm_win->w * (size_t)s_wm_win->h);
            }
            /* Flush initial wm_surface_dirty before presenting crisp 1:1 HD frames. */
            wm_compositor_tick();
            s_force_hw_reflip = true;
        } else {
            init_mx = s_gui_w / 2;
            init_my = s_gui_h / 2;
        }
    } else {
        init_mx = s_wm_win ? (ms.x - s_wm_win->x) : ms.x;
        init_my = s_wm_win ? (ms.y - s_wm_win->y) : ms.y;
    }
    s_mx = (init_mx >= 0 && init_mx < s_gui_w) ? init_mx : (s_gui_w / 2);
    s_my = (init_my >= 0 && init_my < s_gui_h) ? init_my : (s_gui_h / 2);
    s_last_flip_mx = s_mx;
    s_last_flip_my = s_my;
    s_last_flip_cur = s_cursor;
    s_btn_l = ms.left_button;
    s_btn_r = ms.right_button;
    s_btn_m = ms.middle_button;
    s_prev_scroll_pos = ms.scroll_pos;

    s_inited = true;
    s_dirty_chrome = true;
    s_dirty_vp = true;
    s_dirty_vp_rows = false;
    s_dirty_vp_y1 = 0;
    s_dirty_vp_y2 = s_gui_h;
    s_vid_last_tick_ms = gui_millis();
    sync_default_font_size(true);
}

void ipo_gui_shutdown(void) {
    if (!s_inited) return;
    s_shutting_down = true;
    s_links_wakeup = false;
    video_stop_audio();
    if (s_mp4_player) { mp4_player_destroy(s_mp4_player); s_mp4_player = NULL; }
    for (int i = 0; i < s_dls_cap; i++) {
        dl_slot_close_conn(&s_dls[i]);
        dl_slot_free_strings(&s_dls[i]);
    }
    free(s_dls); s_dls = NULL; s_dls_cap = 0;
    for (int i = 0; i < s_bookmark_count; i++) {
        free(s_bookmarks[i].label);
        free(s_bookmarks[i].url);
    }
    free(s_bookmarks); s_bookmarks = NULL; s_bookmark_cap = 0; s_bookmark_count = 0;

    if (s_wm_win) {
        if (wm_is_window_valid(s_wm_win)) wm_destroy_window(s_wm_win);
        s_wm_win = NULL;
    }
    if (s_direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
        s_direct_gfx = false;
    }

    free(s_wm_lut_x); s_wm_lut_x = NULL; s_wm_lut_x_w = 0; s_wm_lut_src_w = 0;
    free(s_fb32);     s_fb32 = NULL;
    free(s_fb8_base); s_fb8_base = NULL;
    free(s_lut_near); s_lut_near = NULL;
    free(s_ovl_save32); s_ovl_save32 = NULL; s_ovl_save_cap = 0;
    for (int i = 0; i < s_tabs_cap; i++) {
        tab_free_fields(&s_tabs[i]);
    }
    free(s_tabs);     s_tabs = NULL; s_tabs_cap = 0;
    for (int i = 0; i < s_history_count; i++) {
        free(s_history[i].title);
        free(s_history[i].url);
    }
    free(s_history);  s_history = NULL; s_history_cap = 0; s_history_count = 0;
    ac_clear_items();
    free(s_ac_items); s_ac_items = NULL; s_ac_cap = 0; s_ac_sel = -1;
    free(s_ctx_link_url); s_ctx_link_url = NULL;
    free(s_omnibox_text); s_omnibox_text = NULL; s_omnibox_cap = 0; s_omnibox_len = 0;
    free(s_hero_text);    s_hero_text = NULL;    s_hero_cap = 0;    s_hero_len = 0;
    free(s_find_text);    s_find_text = NULL;    s_find_cap = 0;    s_find_len = 0;
    free(s_status_msg);   s_status_msg = NULL;
    free(s_pending_initial_video); s_pending_initial_video = NULL;
    free(s_pending_tab_url);       s_pending_tab_url = NULL;
    s_inited = false;
}

void ipo_gui_on_device_created(struct graphics_device *dev) {
    int slot = ensure_tab_slot();
    if (slot < 0) return;

    ipo_tab_t *tab = &s_tabs[slot];
    tab_free_fields(tab);
    memset(tab, 0, sizeof(*tab));
    tab->used = true;
    tab->dev = dev;
    tab->is_home = true;
    gui_set_str(&tab->title, "New Tab");
    s_active_tab = slot;
    set_omnibox_str("");

    if (s_pending_initial_video && s_pending_initial_video[0]) {
        char *vu = s_pending_initial_video;
        s_pending_initial_video = NULL;
        enter_video_in_tab(slot, vu);
        free(vu);
    } else if (s_pending_tab_url && s_pending_tab_url[0]) {
        char *tu = s_pending_tab_url;
        s_pending_tab_url = NULL;
        if (strcmp(tu, "about:home") != 0) navigate_tab_to(slot, tu);
        free(tu);
    }
    s_dirty_chrome = true;
    s_dirty_vp = true;
    render_and_present();
}

void ipo_gui_on_device_destroyed(struct graphics_device *dev) {
    int idx = -1;
    for (int i = 0; i < s_tabs_cap; i++) {
        if (s_tabs[i].used && s_tabs[i].dev == dev) { idx = i; break; }
    }
    if (idx < 0) return;
    if (s_tabs[idx].is_video && idx == s_active_tab) video_stop_audio();
    tab_free_fields(&s_tabs[idx]);
    memset(&s_tabs[idx], 0, sizeof(s_tabs[idx]));

    if (idx == s_active_tab) {
        int next = -1;
        for (int i = 0; i < s_tabs_cap; i++) {
            if (s_tabs[i].used) { next = i; break; }
        }
        if (next >= 0) {
            s_active_tab = -1;
            switch_to_tab(next);
        } else {
            terminate_loop = 1;
        }
    } else {
        s_dirty_chrome = true;
    }
}

struct graphics_device *ipo_gui_active_dev(void) {
    return (s_tabs && s_active_tab >= 0 && s_active_tab < s_tabs_cap) ? s_tabs[s_active_tab].dev : NULL;
}

void ipo_gui_set_title(struct graphics_device *dev, const char *title) {
    if (!dev || !s_inited || s_shutting_down || !s_tabs) return;
    for (int i = 0; i < s_tabs_cap; i++) {
        if (!s_tabs[i].used || s_tabs[i].dev != dev) continue;
        if (s_tabs[i].is_home || s_tabs[i].is_video) return;
        if (title && title[0]) {
            gui_set_str(&s_tabs[i].title, title);
            if (i == s_active_tab && s_wm_win && !s_hw_direct_fb) {
                char *wtitle = gui_fmt_alloc("%s - Links", gui_str(s_tabs[i].title));
                if (wtitle) {
                    wm_set_title(s_wm_win, wtitle);
                    free(wtitle);
                }
            }
            s_dirty_chrome = true;
        }
        return;
    }
}

int ipo_gui_is_viewport_live(struct graphics_device *dev) {
    if (!dev || !s_inited || s_shutting_down || !s_tabs || s_active_tab < 0 || s_active_tab >= s_tabs_cap) return 0;
    ipo_tab_t *tab = &s_tabs[s_active_tab];
    return (tab->used && tab->dev == dev && !tab->is_home && !tab->is_video) ? 1 : 0;
}

int ipo_gui_vp_x(void) { return 0; }
int ipo_gui_vp_y(void) { return s_fullscreen ? 0 : IPO_TOP_CHROME; }
int ipo_gui_vp_w(void) { return IPO_GUI_W; }
int ipo_gui_vp_h(void) { return s_fullscreen ? IPO_GUI_H : (IPO_GUI_H - IPO_TOP_CHROME - IPO_STATUSBAR_H); }

uint32_t *ipo_gui_fb32(void) { return s_fb32; }
void ipo_gui_mark_vp_dirty(void) { s_dirty_vp = true; }
void ipo_gui_mark_vp_dirty_rows(int y1, int y2) {
    if (y1 < 0) y1 = 0;
    if (y2 > s_gui_h) y2 = s_gui_h;
    if (y1 >= y2) return;
    if (!s_dirty_vp_rows) {
        s_dirty_vp_rows = true;
        s_dirty_vp_y1 = y1;
        s_dirty_vp_y2 = y2;
    } else {
        if (y1 < s_dirty_vp_y1) s_dirty_vp_y1 = y1;
        if (y2 > s_dirty_vp_y2) s_dirty_vp_y2 = y2;
    }
}

void ipo_gui_on_scroll(int x1, int y1, int x2, int y2, int scx, int scy) {
    if (!s_fb32 || !s_fb8_base) return;
    int gw = s_gui_w;
    int gh = s_gui_h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > gw) x2 = gw;
    if (y2 > gh) y2 = gh;
    int bw = x2 - x1;
    int bh = y2 - y1;
    if (bw <= 0 || bh <= 0) return;

    int ax = (scx < 0) ? -scx : scx;
    int ay = (scy < 0) ? -scy : scy;
    if (ax >= bw || ay >= bh) {
        ipo_gui_mark_vp_dirty_rows(y1, y2);
        return;
    }

    int src_x = (scx >= 0) ? x1 : (x1 - scx);
    int dst_x = (scx >= 0) ? (x1 + scx) : x1;
    int copy_w = bw - ax;
    int copy_h = bh - ay;

    if (scy > 0) {
        for (int r = copy_h - 1; r >= 0; r--) {
            int sy = y1 + r;
            int dy = sy + scy;
            memmove(s_fb32 + dy * gw + dst_x, s_fb32 + sy * gw + src_x, (size_t)copy_w * sizeof(uint32_t));
            memmove(s_fb8_base + dy * gw + dst_x, s_fb8_base + sy * gw + src_x, (size_t)copy_w);
        }
        ipo_gui_mark_vp_dirty_rows(y1, y1 + scy);
    } else {
        for (int r = 0; r < copy_h; r++) {
            int sy = y1 - scy + r;
            int dy = y1 + r;
            memmove(s_fb32 + dy * gw + dst_x, s_fb32 + sy * gw + src_x, (size_t)copy_w * sizeof(uint32_t));
            memmove(s_fb8_base + dy * gw + dst_x, s_fb8_base + sy * gw + src_x, (size_t)copy_w);
        }
        if (scy < 0) {
            ipo_gui_mark_vp_dirty_rows(y2 + scy, y2);
        } else {
            ipo_gui_mark_vp_dirty_rows(y1, y2);
        }
    }
}

void ipo_gui_mark_chrome_dirty(void) { s_dirty_chrome = true; }

void ipo_gui_present_now(void) {
    render_and_present();
}

void ipo_gui_notify_status(void *ses_ptr) {
    struct session *ses = (struct session *)ses_ptr;
    if (!ses) return;
    if (ses->st && ses->st[0]) {
        gui_set_str(&s_status_msg, (const char *)ses->st);
        s_dirty_chrome = true;
    }
    sync_active_tab_meta();
}

void ipo_gui_notify_js_console(const char *msg) {
    if (!msg || !*msg) return;
    fprintf(stderr, "[JS console] %s\n", msg);
    if (s_inited && !s_shutting_down) {
        gui_set_fmt(&s_status_msg, "JS: %s", msg);
        s_dirty_chrome = true;
    }
}

int ipo_gui_dom_dirty_hint(void) {
    return 0;
}

static int modal_process_raw_scancode(uint8_t raw, int kind,
                                      char **input_buf, size_t *input_cap,
                                      int *input_len, int *input_cur, bool *input_sel_all,
                                      bool *e0, bool *shift_held, bool *ctrl_held,
                                      bool *need_draw) {
    if (raw == 0xE0) { *e0 = true; return -1; }
    bool rel = (raw & 0x80u) != 0;
    uint8_t sc = raw & 0x7Fu;
    bool ext = *e0;
    *e0 = false;
    if (sc == 0x2A || sc == 0x36) { *shift_held = !rel; return -1; }
    if (sc == 0x1D) { *ctrl_held = !rel; return -1; }
    if (rel) return -1;
    if (sc == 0x01) return (kind == 0) ? 1 : 0; /* Esc */
    if (sc == 0x1C) return 1; /* Enter */
    if (kind == 2) {
        bool ent = false;
        uint8_t hmods = keyboard_get_modifiers();
        if (handle_textbox_key(input_buf, input_cap, input_len, input_cur,
                               input_sel_all, sc, ext,
                               *ctrl_held || ((hmods & KBD_MOD_CTRL) != 0),
                               *shift_held || ((hmods & KBD_MOD_SHIFT) != 0),
                               &ent)) {
            if (ent) return 1;
            *need_draw = true;
        }
    }
    return -1;
}

int ipo_gui_run_js_dialog(int kind, const char *title, const char *message,
                          const char *default_text, char **out_alloc) {
    if (out_alloc) *out_alloc = NULL;
    fprintf(stderr, "[JS %s] %s\n",
            kind == 0 ? "alert" : (kind == 1 ? "confirm" : "prompt"),
            message ? message : "");
    if (!s_inited || s_shutting_down || !s_fb32 || !s_fb8_base) {
        if (kind == 2 && out_alloc) {
            *out_alloc = gui_fmt_alloc("%s", default_text ? default_text : "");
        }
        return 1;
    }

    s_dirty_chrome = true;
    render_and_present();

    bool compact = gui_is_compact();
    int cw = ui_char_w();
    int ch = ui_char_h();
    int dw = compact ? 280 : 420;
    if (dw > s_gui_w - 20) dw = s_gui_w - 20;
    if (dw < 200) dw = s_gui_w;

    int hdr_h = compact ? 22 : 30;
    int foot_h = compact ? 28 : 36;
    int prompt_h = (kind == 2) ? (ch + 16) : 0;
    int max_cols = (dw - 28) / (cw > 0 ? cw : 6);
    if (max_cols < 16) max_cols = 16;

    /* Word-wrap message dynamically to fit available dialog height without fixed line caps */
    int line_step = ch + 4;
    int avail_body = (s_gui_h - 16) - hdr_h - foot_h - 18 - prompt_h;
    int max_vis_lines = (line_step > 0 && avail_body > line_step) ? (avail_body / line_step) : 1;
    char **lines = NULL;
    int nlines = 0, lines_cap = 0;
    const char *mp = message ? message : "";
    while (*mp) {
        while (*mp == ' ') mp++;
        if (!*mp) break;
        int take = 0, last_sp = -1;
        while (mp[take] && mp[take] != '\n' && take < max_cols) {
            if (mp[take] == ' ') last_sp = take;
            take++;
        }
        if (mp[take] && mp[take] != '\n' && last_sp > 0) take = last_sp;
        if (nlines >= lines_cap) {
            int ncap = lines_cap ? lines_cap * 2 : 8;
            if (ncap <= lines_cap) break;
            char **nl = (char **)realloc(lines, (size_t)ncap * sizeof(char *));
            if (!nl) break;
            lines = nl;
            lines_cap = ncap;
        }
        char *ln = NULL;
        if (!gui_set_strn(&ln, mp, (size_t)take)) break;
        lines[nlines++] = ln;
        mp += take;
        if (*mp == '\n') mp++;
    }
    if (nlines == 0) {
        char *blank = gui_fmt_alloc(" ");
        if (blank) {
            char **nl = (char **)realloc(lines, sizeof(char *));
            if (nl) { lines = nl; lines_cap = 1; lines[nlines++] = blank; }
            else free(blank);
        }
    }
    int draw_lines = (nlines < max_vis_lines) ? nlines : max_vis_lines;
    if (draw_lines < 1 && nlines > 0) draw_lines = 1;

    int body_h = draw_lines * line_step + prompt_h;
    int dh = hdr_h + body_h + foot_h + 18;
    if (dh > s_gui_h - 16) dh = s_gui_h - 16;
    int dx = (s_gui_w - dw) / 2;
    int dy = (s_gui_h - dh) / 2;

    uint32_t *saved_bg = (uint32_t *)malloc((size_t)dw * (size_t)dh * sizeof(uint32_t));
    if (saved_bg) {
        for (int r = 0; r < dh; r++) {
            memcpy(saved_bg + r * dw, s_fb32 + (dy + r) * s_gui_w + dx, (size_t)dw * sizeof(uint32_t));
        }
    }

    char *input_buf = NULL;
    size_t input_cap = 0;
    int input_len = 0;
    gui_dyn_set(&input_buf, &input_cap, &input_len, default_text ? default_text : "");
    int input_cur = input_len;
    bool input_sel_all = (input_len > 0);
    bool e0 = false, shift_held = false, ctrl_held = false;
    int btn_w = compact ? 64 : 84;
    int btn_h = compact ? 20 : 24;
    int btn_y = dy + dh - btn_h - 10;
    int ok_x = (kind == 0) ? (dx + dw - btn_w - 14) : (dx + dw - btn_w * 2 - 22);
    int can_x = dx + dw - btn_w - 12;
    int inp_x = dx + 14;
    int inp_y = dy + hdr_h + 8 + draw_lines * line_step + 2;
    int inp_w = dw - 28;
    int inp_h = ch + 10;
    int hov_btn = 0; /* 1=OK, 2=Cancel, 3=Close */
    int result = -1;
    bool prev_l = s_btn_l;
    bool need_draw = true;

    while (result < 0 && !terminate_loop && !s_shutting_down) {
        if (need_draw) {
            fill_rrect32(dx, dy, dw, dh, 8, COL_TABBAR_BG);
            stroke_rrect32(dx, dy, dw, dh, 8, COL_ACCENT_BLUE);
            draw_text_clip32(dx + 12, dy + (hdr_h - ch) / 2,
                             (title && *title) ? title : "JavaScript",
                             COL_ACCENT_CYAN, dw - 40, 1);
            int cls_s = compact ? 14 : 18;
            fill_rrect32(dx + dw - cls_s - 6, dy + (hdr_h - cls_s) / 2, cls_s, cls_s, 4,
                         (hov_btn == 3) ? COL_ACCENT_RED : COL_CARD_BG);
            draw_icon_close(dx + dw - 6 - cls_s / 2, dy + hdr_h / 2, compact ? 2 : 3, COL_TEXT_MAIN);
            fill_rect32(dx + 8, dy + hdr_h, dw - 16, 1, COL_BORDER);

            for (int i = 0; i < draw_lines; i++) {
                draw_text_clip32(dx + 14, dy + hdr_h + 8 + i * line_step,
                                 lines[i], COL_TEXT_MAIN, dw - 28, 1);
            }

            if (kind == 2) {
                fill_rrect32(inp_x, inp_y, inp_w, inp_h, 4, COL_OMNIBOX_BG);
                stroke_rrect32(inp_x, inp_y, inp_w, inp_h, 4, COL_ACCENT_BLUE);
                if (input_sel_all && input_len > 0) {
                    int total_cols = utf8_total_cols(gui_str(input_buf), input_len);
                    int sw = ui_substr_w(gui_str(input_buf), input_len, 0, total_cols);
                    if (sw > inp_w - 12) sw = inp_w - 12;
                    fill_rect32(inp_x + 6, inp_y + 3, sw, ch + 4, 0x1D4ED8u);
                }
                draw_text_clip32(inp_x + 6, inp_y + 5, gui_str(input_buf), COL_TEXT_MAIN, inp_w - 12, 1);
                int cur_cols = utf8_byte_to_col(gui_str(input_buf), input_cur);
                int cxp = inp_x + 6 + ui_substr_w(gui_str(input_buf), input_len, 0, cur_cols);
                if (cxp < inp_x + inp_w - 6) {
                    fill_rect32(cxp, inp_y + 4, 1, ch + 2, COL_WHITE);
                }
            }

            fill_rrect32(ok_x, btn_y, btn_w, btn_h, 4,
                         (hov_btn == 1) ? 0x3B82F6u : COL_ACCENT_BLUE);
            int ok_tw = ui_text_w("OK");
            draw_text_clip32(ok_x + (btn_w - ok_tw) / 2, btn_y + (btn_h - ch) / 2,
                             "OK", COL_WHITE, btn_w - 4, 1);

            if (kind != 0) {
                fill_rrect32(can_x, btn_y, btn_w, btn_h, 4,
                             (hov_btn == 2) ? COL_BTN_HOVER : COL_CARD_BG);
                stroke_rrect32(can_x, btn_y, btn_w, btn_h, 4, COL_CARD_BD);
                int c_tw = ui_text_w("Cancel");
                draw_text_clip32(can_x + (btn_w - c_tw) / 2, btn_y + (btn_h - ch) / 2,
                                 "Cancel", COL_TEXT_MAIN, btn_w - 4, 1);
            }

            quantize_rect(dx, dy, dx + dw, dy + dh, false);
            flip_output(false);
            need_draw = false;
        }

        if (!s_wm_win) {
            if (system_is_interrupted()) {
                terminate_loop = 1;
                result = 0;
            } else {
                keyboard_poll();
                uint8_t raw;
                while (result < 0 && (raw = keyboard_get_scancode()) != 0) {
                    int r = modal_process_raw_scancode(raw, kind, &input_buf, &input_cap,
                                                       &input_len, &input_cur, &input_sel_all,
                                                       &e0, &shift_held, &ctrl_held, &need_draw);
                    if (r >= 0) { result = r; break; }
                }
            }
        } else {
            wm_event_t ev;
            while (wm_poll_event(s_wm_win, &ev)) {
                if (ev.type == WM_EVENT_CLOSE) {
                    terminate_loop = 1;
                    result = 0;
                    break;
                }
                if (ev.type == WM_EVENT_KEY_DOWN || ev.type == WM_EVENT_KEY_UP) {
                    int r = modal_process_raw_scancode((uint8_t)(ev.data & 0xFFu), kind,
                                                       &input_buf, &input_cap,
                                                       &input_len, &input_cur, &input_sel_all,
                                                       &e0, &shift_held, &ctrl_held, &need_draw);
                    if (r >= 0) { result = r; break; }
                }
            }
        }
        if (result >= 0) break;

        mouse_state_t ms;
        mouse_get_state(&ms);
        int mx = s_mx, my = s_my;
        bool use_rel_hd = (s_hw_direct_fb && s_wm_win) ||
                          (s_direct_gfx && (s_gui_w > 320 || s_gui_h > 200));
        if (use_rel_hd) {
            int64_t cm_x = s_prev_cm_x, cm_y = s_prev_cm_y;
            mouse_get_cumulative_motion(&cm_x, &cm_y);
            int64_t mdx = cm_x - s_prev_cm_x;
            int64_t mdy = cm_y - s_prev_cm_y;
            s_prev_cm_x = cm_x;
            s_prev_cm_y = cm_y;
            bool focused = true;
            if (s_wm_win) {
                focused = (wm_get_focused() == s_wm_win);
                if (focused && (ms.x != s_prev_wm_mx || ms.y != s_prev_wm_my)) {
                    wm_compositor_tick();
                    s_prev_wm_mx = ms.x;
                    s_prev_wm_my = ms.y;
                    s_force_hw_reflip = true;
                }
            }
            if (focused) {
                int64_t nx = (int64_t)s_mx + mdx;
                int64_t ny = (int64_t)s_my + mdy;
                if (nx < 0) nx = 0; else if (nx >= s_gui_w) nx = s_gui_w - 1;
                if (ny < 0) ny = 0; else if (ny >= s_gui_h) ny = s_gui_h - 1;
                mx = (int)nx;
                my = (int)ny;
            }
        } else {
            mx = s_wm_win ? (ms.x - s_wm_win->x) : ms.x;
            my = s_wm_win ? (ms.y - s_wm_win->y) : ms.y;
            if (mx < 0) mx = 0; else if (mx >= s_gui_w) mx = s_gui_w - 1;
            if (my < 0) my = 0; else if (my >= s_gui_h) my = s_gui_h - 1;
        }
        bool cur_moved = (mx != s_mx || my != s_my);
        s_mx = mx; s_my = my;
        if ((cur_moved || s_force_hw_reflip) && !need_draw) flip_output(true);

        int cls_s = compact ? 14 : 18;
        int nhov = 0;
        if (mx >= ok_x && mx < ok_x + btn_w && my >= btn_y && my < btn_y + btn_h) nhov = 1;
        else if (kind != 0 && mx >= can_x && mx < can_x + btn_w && my >= btn_y && my < btn_y + btn_h) nhov = 2;
        else if (mx >= dx + dw - cls_s - 6 && mx < dx + dw - 6 &&
                 my >= dy + (hdr_h - cls_s) / 2 && my < dy + (hdr_h + cls_s) / 2) nhov = 3;
        if (nhov != hov_btn) { hov_btn = nhov; need_draw = true; }

        if (ms.left_button != prev_l) {
            prev_l = ms.left_button;
            if (!ms.left_button) { /* activate on release or press; handle press below */ }
            else {
                if (nhov == 1) result = 1;
                else if (nhov == 2) result = 0;
                else if (nhov == 3) result = (kind == 0) ? 1 : 0;
            }
        }
        if (result < 0) usleep(12000);
    }

    for (int i = 0; i < nlines; i++) free(lines[i]);
    free(lines);
    s_btn_l = prev_l;
    if (saved_bg) {
        for (int r = 0; r < dh; r++) {
            memcpy(s_fb32 + (dy + r) * s_gui_w + dx, saved_bg + r * dw, (size_t)dw * sizeof(uint32_t));
        }
        free(saved_bg);
        quantize_rect(dx, dy, dx + dw, dy + dh, false);
        flip_output(false);
    }
    s_dirty_chrome = true;
    s_dirty_vp = true;

    if (kind == 2 && result == 1 && out_alloc) {
        *out_alloc = input_buf;
    } else {
        free(input_buf);
    }
    return result == 1 ? 1 : 0;
}

int ipo_gui_maybe_intercept_url(void *ses_ptr, const char *url) {
    if (!s_inited || !url || !*url || !s_tabs) return 0;
    struct session *ses = (struct session *)ses_ptr;
    int target_tab = s_active_tab;
    if (ses && ses->term) {
        for (int i = 0; i < s_tabs_cap; i++) {
            if (s_tabs[i].used && s_tabs[i].dev == ses->term->dev) {
                target_tab = i;
                break;
            }
        }
    }

    if (strcmp(url, "about:home") == 0 || strcmp(url, "links://home") == 0) {
        if (target_tab >= 0) navigate_tab_to(target_tab, "about:home");
        return 1;
    }
    if (strncasecmp(url, "javascript:", 11) == 0) {
        return 0;
    }
    if (yt_classify_url(url) == YT_URL_VIDEO) {
        enter_video_in_tab(target_tab, url);
        return 1;
    }

    /* Regular HTTP/HTTPS navigation (e.g., from CLI args or link click) */
    if (target_tab >= 0 && target_tab < s_tabs_cap && s_tabs[target_tab].used) {
        if (s_tabs[target_tab].is_home || s_tabs[target_tab].is_video) {
            if (s_tabs[target_tab].is_video) video_stop_audio();
            s_tabs[target_tab].is_home = false;
            s_tabs[target_tab].is_video = false;
            fill_rect32(ipo_gui_vp_x(), ipo_gui_vp_y(), ipo_gui_vp_w(), ipo_gui_vp_h(), COL_WHITE);
            s_dirty_vp = true;
        }
        gui_set_str(&s_tabs[target_tab].url, url);
        record_history_visit(url, s_tabs[target_tab].title);
        if (target_tab == s_active_tab && s_focus != FOCUS_OMNIBOX) {
            set_omnibox_str(url);
        }
        s_dirty_chrome = true;
    }
    return 0;
}

static void sync_window_size(void) {
    int nw = s_gui_w;
    int nh = s_gui_h;
    if (s_wm_win) {
        if (!wm_is_window_valid(s_wm_win)) return;
        if (s_use_truecolor32 && s_wm_win->fullscreen) {
            vga_gfx_get_screen_bounds(&nw, &nh);
        } else if (s_hw_direct_fb && s_wm_win->fullscreen) {
            vga_gfx_probe_screen_size();
            vga_gfx_get_screen_bounds(&nw, &nh);
            if (nw <= (int)s_wm_win->w && nh <= (int)s_wm_win->h) {
                s_hw_direct_fb = false;
                nw = (int)s_wm_win->w;
                nh = (int)s_wm_win->h;
            }
        } else {
            int px = 0, py = 0, pw = 0, ph = 0;
            if (s_use_truecolor32) ipo_gfx_wm_window_phys_rect(s_wm_win, &px, &py, &pw, &ph);
            if (s_use_truecolor32 && pw > 32 && ph > 32) {
                nw = pw;
                nh = ph;
            } else {
                nw = (int)s_wm_win->w;
                nh = (int)s_wm_win->h;
            }
        }
    } else if (s_direct_gfx) {
        vga_gfx_get_screen_bounds(&nw, &nh);
    } else {
        return;
    }
    if (nw <= 0 || nh <= 0 || (nw == s_gui_w && nh == s_gui_h)) return;

    size_t npx = (size_t)nw * (size_t)nh;
    uint32_t *nfb32 = (uint32_t *)calloc(npx, sizeof(uint32_t));
    uint8_t  *nfb8  = (uint8_t  *)calloc(npx, 1);
    if (!nfb32 || !nfb8) {
        free(nfb32);
        free(nfb8);
        return;
    }

    free(s_fb32);
    free(s_fb8_base);
    s_fb32 = nfb32;
    s_fb8_base = nfb8;
    s_gui_w = nw;
    s_gui_h = nh;
    s_wm_last_w = s_wm_win ? (int)s_wm_win->w : nw;
    s_wm_last_h = s_wm_win ? (int)s_wm_win->h : nh;

    if (s_mx >= nw) s_mx = nw - 1;
    if (s_my >= nh) s_my = nh - 1;

    int vpw = ipo_gui_vp_w();
    int vph = ipo_gui_vp_h();
    ipo_driver.x = vpw;
    ipo_driver.y = vph;

    for (int i = 0; i < s_tabs_cap; i++) {
        if (!s_tabs[i].used || !s_tabs[i].dev) continue;
        struct graphics_device *dev = s_tabs[i].dev;
        dev->size.x1 = 0;
        dev->size.y1 = 0;
        dev->size.x2 = vpw;
        dev->size.y2 = vph;
        set_clip_area(dev, &dev->size);
        if (dev->resize_handler) dev->resize_handler(dev);
    }

    s_dirty_chrome = true;
    s_dirty_vp = true;
    s_dirty_vp_rows = false;
    s_dirty_vp_y1 = 0;
    s_dirty_vp_y2 = nh;
    s_dirty_overlay = true;
    s_links_wakeup = true;
    sync_default_font_size(false);
}

void ipo_gui_tick(void) {
    if (!s_inited || s_shutting_down || !s_tabs) return;

    if (s_wm_win) {
        if (!wm_is_window_valid(s_wm_win) || !wm_session_active()) {
            terminate_loop = 1;
            return;
        }
    } else if (!s_direct_gfx || system_is_interrupted()) {
        terminate_loop = 1;
        return;
    }

    /* 1. Pump non-blocking downloads */
    dl_tick_all();

    /* 2. Drain event queue (keyboard / close / resize) */
    poll_wm_events();
    if (terminate_loop) return;
    sync_window_size();

    /* 3. Poll PS/2 mouse relative to browser surface */
    mouse_state_t ms;
    mouse_get_state(&ms);
    int mx = s_mx, my = s_my;
    bool in_win;
    bool use_rel_hd = (s_hw_direct_fb && s_wm_win) ||
                      (s_direct_gfx && (s_gui_w > 320 || s_gui_h > 200));
    if (use_rel_hd) {
        int64_t cm_x = s_prev_cm_x, cm_y = s_prev_cm_y;
        mouse_get_cumulative_motion(&cm_x, &cm_y);
        int64_t dx = cm_x - s_prev_cm_x;
        int64_t dy = cm_y - s_prev_cm_y;
        s_prev_cm_x = cm_x;
        s_prev_cm_y = cm_y;
        bool focused = true;
        if (s_wm_win) {
            focused = (wm_get_focused() == s_wm_win);
            if (focused && (ms.x != s_prev_wm_mx || ms.y != s_prev_wm_my)) {
                wm_compositor_tick();
                poll_wm_events();
                if (terminate_loop) return;
                s_prev_wm_mx = ms.x;
                s_prev_wm_my = ms.y;
                s_force_hw_reflip = true;
            }
        }
        if (focused) {
            int64_t nx = (int64_t)s_mx + dx;
            int64_t ny = (int64_t)s_my + dy;
            if (nx < 0) nx = 0; else if (nx >= IPO_GUI_W) nx = IPO_GUI_W - 1;
            if (ny < 0) ny = 0; else if (ny >= IPO_GUI_H) ny = IPO_GUI_H - 1;
            mx = (int)nx;
            my = (int)ny;
        }
        in_win = focused;
    } else {
        int cw, ch, rx, ry;
        if (s_wm_win) {
            cw = (s_wm_win->w > 1) ? s_wm_win->w : 1;
            ch = (s_wm_win->h > 1) ? s_wm_win->h : 1;
            rx = ms.x - s_wm_win->x;
            ry = ms.y - s_wm_win->y;
            in_win = (wm_get_focused() == s_wm_win) && (rx >= 0 && rx < cw && ry >= 0 && ry < ch);
        } else {
            cw = (s_gui_w > 1) ? s_gui_w : 1;
            ch = (s_gui_h > 1) ? s_gui_h : 1;
            rx = ms.x;
            ry = ms.y;
            in_win = true;
        }
        mx = (rx * IPO_GUI_W) / cw;
        my = (ry * IPO_GUI_H) / ch;
        if (mx < 0) mx = 0; else if (mx >= IPO_GUI_W) mx = IPO_GUI_W - 1;
        if (my < 0) my = 0; else if (my >= IPO_GUI_H) my = IPO_GUI_H - 1;
    }
    if (!in_win && !s_dragging_vscroll && !s_vid_dragging_seek && !s_dragging_textbox) {
        ms.left_button = 0;
        ms.middle_button = 0;
        ms.right_button = 0;
    }

    bool moved = (mx != s_mx || my != s_my);
    s_mx = mx;
    s_my = my;

    cursor_kind_t next_cur = CURSOR_ARROW;
    int hit = hit_test_ui(mx, my, &next_cur);
    if (hit != s_hover_id || next_cur != s_cursor) {
        int prev_hit = s_hover_id;
        s_hover_id = hit;
        s_cursor = next_cur;
        if ((prev_hit >= 100 && prev_hit < 3000) || (hit >= 100 && hit < 3000) ||
            (s_fullscreen && my < 45)) {
            s_dirty_chrome = true;
        }
        if ((prev_hit >= 500 && prev_hit < 800) || (hit >= 500 && hit < 800)) {
            s_dirty_overlay = true;
        }
        if (s_tabs[s_active_tab].is_home &&
            ((prev_hit >= 900 && prev_hit < 920) || (hit >= 900 && hit < 920))) {
            s_dirty_vp = true;
        }
    }

    ipo_tab_t *tab = &s_tabs[s_active_tab];
    bool in_vp = (!s_fullscreen && my >= IPO_TOP_CHROME && my < IPO_GUI_H - IPO_STATUSBAR_H) || s_fullscreen;
    if (hit >= 500 && hit < 800) in_vp = false;

    /* Left mouse button transitions */
    if (ms.left_button != s_btn_l) {
        s_btn_l = ms.left_button;
        s_links_wakeup = true;
        if (s_btn_l) {
            if (hit > 0 || !in_vp || s_overlay != OVL_NONE || (s_focus == FOCUS_OMNIBOX && s_ac_count > 0)) {
                handle_ui_click(hit, mx, my);
            } else if (tab->used && !tab->is_home && !tab->is_video &&
                       tab->dev && tab->dev->mouse_handler) {
                s_focus = FOCUS_VIEWPORT;
                s_dirty_chrome = true;
                tab->dev->mouse_handler(tab->dev, mx - ipo_gui_vp_x(), my - ipo_gui_vp_y(),
                                        B_DOWN | B_LEFT);
            }
        } else {
            if (s_vid_dragging_seek && s_mp4_player) {
                mp4_player_seek_ms(s_mp4_player, s_vid_pos_ms);
            }
            s_vid_dragging_seek = 0;
            s_dragging_vscroll = 0;
            s_dragging_textbox = 0;
            if (in_vp && tab->used && !tab->is_home && !tab->is_video &&
                tab->dev && tab->dev->mouse_handler) {
                tab->dev->mouse_handler(tab->dev, mx - ipo_gui_vp_x(), my - ipo_gui_vp_y(),
                                        B_UP | B_LEFT);
            }
        }
    } else if (s_btn_l && s_dragging_textbox) {
        static uint32_t s_last_tb_drag_ms = 0;
        uint32_t tnow = gui_millis();
        if (moved || (uint32_t)(tnow - s_last_tb_drag_ms) >= 55u) {
            s_last_tb_drag_ms = tnow;
            textbox_handle_drag(mx);
        }
    } else if (moved) {
        if (s_dragging_vscroll) {
            scroll_viewport_to_mouse_y(my);
        } else if (s_vid_dragging_seek && tab->is_video) {
            video_layout_t V;
            compute_video_layout(ipo_gui_vp_x(), ipo_gui_vp_y(), ipo_gui_vp_w(), ipo_gui_vp_h(), &V);
            int rx = mx - V.sb_x;
            if (rx < 0) rx = 0; else if (rx > V.sb_w) rx = V.sb_w;
            uint32_t dur = s_mp4_player ? mp4_player_duration_ms(s_mp4_player) : 60000u;
            s_vid_pos_ms = (V.sb_w > 0) ? (uint32_t)(((uint64_t)rx * (uint64_t)dur) / (uint64_t)V.sb_w) : 0u;
            s_dirty_vp = true;
        } else if (in_vp && hit == 0 && tab->used && !tab->is_home && !tab->is_video &&
                   tab->dev && tab->dev->mouse_handler) {
            int btn = s_btn_l ? (B_DRAG | B_LEFT) : (B_MOVE | B_LEFT);
            tab->dev->mouse_handler(tab->dev, mx - ipo_gui_vp_x(), my - ipo_gui_vp_y(), btn);
        }
    }

    /* Middle mouse button (close tab on tab bar, or open link in new tab) */
    if (ms.middle_button != s_btn_m) {
        s_btn_m = ms.middle_button;
        if (s_btn_m) {
            if (hit >= 1000 && hit < 1000 + s_tabs_cap) {
                close_tab(hit - 1000);
            } else if (in_vp && tab->used && !tab->is_home && !tab->is_video && tab->dev) {
                if (tab->dev->mouse_handler) {
                    tab->dev->mouse_handler(tab->dev, mx - ipo_gui_vp_x(), my - ipo_gui_vp_y(), B_MOVE | B_LEFT);
                }
                struct session *ses = dev_to_session(tab->dev);
                struct f_data_c *fd = ses ? current_frame(ses) : NULL;
                if (fd && fd->f_data && fd->vs && fd->vs->current_link >= 0 &&
                    fd->vs->current_link < fd->f_data->nlinks) {
                    struct link *lnk = &fd->f_data->links[fd->vs->current_link];
                    if (lnk->where && lnk->where[0]) {
                        open_new_tab((const char *)lnk->where);
                    }
                }
            }
        }
    }

    /* Right mouse button -> Modern Browser Context Menu */
    if (ms.right_button != s_btn_r) {
        s_btn_r = ms.right_button;
        if (s_btn_r && in_vp) {
            s_ctx_has_link = false;
            free(s_ctx_link_url);
            s_ctx_link_url = NULL;
            if (tab->used && !tab->is_home && !tab->is_video && tab->dev) {
                if (tab->dev->mouse_handler) {
                    tab->dev->mouse_handler(tab->dev, mx - ipo_gui_vp_x(), my - ipo_gui_vp_y(), B_MOVE | B_LEFT);
                }
                struct session *ses = dev_to_session(tab->dev);
                struct f_data_c *fd = ses ? current_frame(ses) : NULL;
                if (fd && fd->f_data && fd->vs && fd->vs->current_link >= 0 &&
                    fd->vs->current_link < fd->f_data->nlinks) {
                    struct link *lnk = &fd->f_data->links[fd->vs->current_link];
                    if (lnk->where && lnk->where[0]) {
                        s_ctx_has_link = true;
                        gui_set_str(&s_ctx_link_url, (const char *)lnk->where);
                    }
                }
            }
            s_ctx_x = mx;
            s_ctx_y = my;
            s_overlay = OVL_CONTEXT;
            s_dirty_overlay = true;
            s_dirty_chrome = true;
        }
    }

    /* Mouse wheel scroll */
    int32_t dwheel = ms.scroll_pos - s_prev_scroll_pos;
    if (dwheel != 0) {
        s_prev_scroll_pos = ms.scroll_pos;
        s_links_wakeup = true;
        if (in_vp && tab->used && !tab->is_home && !tab->is_video &&
            tab->dev && tab->dev->mouse_handler) {
            int steps = dwheel > 0 ? dwheel : -dwheel;
            if (steps > 4) steps = 4;
            int wb = (dwheel > 0) ? B_WHEELUP : B_WHEELDOWN;
            for (int w = 0; w < steps; w++) {
                tab->dev->mouse_handler(tab->dev, mx - ipo_gui_vp_x(), my - ipo_gui_vp_y(), wb);
            }
            s_dirty_vp = true;
        }
    }

    /* 4. Video timeline & progressive MP4 H.264/AAC playback tick */
    uint32_t now = gui_millis();
    if (tab->used && tab->is_video && s_mp4_player) {
        video_layout_t V;
        compute_video_layout(ipo_gui_vp_x(), ipo_gui_vp_y(), ipo_gui_vp_w(), ipo_gui_vp_h(), &V);
        mp4_player_tick(s_mp4_player, now, V.vid_w, V.vid_h);
        if (!s_vid_dragging_seek) {
            s_vid_pos_ms = mp4_player_pos_ms(s_mp4_player);
        }
        if (mp4_player_take_dirty(s_mp4_player)) {
            s_dirty_vp = true;
        }
        s_vid_last_tick_ms = now;
    }

    /* Animate loading progress bar & tab loading spinner smoothly */
    if (is_tab_loading(s_active_tab)) {
        s_dirty_chrome = true;
    }

    /* 5. Caret blink or dirty composite / cursor fast-flip */
    static bool s_prev_blink = false;
    bool cur_blink = ((now / 450u) & 1u) != 0;
    if (cur_blink != s_prev_blink && s_focus != FOCUS_VIEWPORT) {
        s_prev_blink = cur_blink;
        if (s_focus == FOCUS_OMNIBOX) s_dirty_chrome = true;
        else if (s_focus == FOCUS_HERO && tab->is_home) s_dirty_vp = true;
        else if (s_focus == FOCUS_FIND && s_overlay == OVL_FIND) s_dirty_overlay = true;
    }

    if (s_dirty_vp || s_dirty_vp_rows || s_dirty_chrome ||
        (s_dirty_overlay && (s_overlay != OVL_NONE || (s_focus == FOCUS_OMNIBOX && s_ac_count > 0)))) {
        render_and_present();
    } else if (s_wm_win && (s_wm_win->w != s_wm_last_w || s_wm_win->h != s_wm_last_h)) {
        flip_output(false);
    } else if (!s_wm_win) {
        int sw = s_gui_w, sh = s_gui_h;
        vga_gfx_get_screen_bounds(&sw, &sh);
        if (sw != s_gui_w || sh != s_gui_h) {
            sync_window_size();
            flip_output(false);
        } else if (s_last_flip_mx != s_mx || s_last_flip_my != s_my || s_last_flip_cur != s_cursor) {
            flip_output(true);
        }
    } else if (s_force_hw_reflip || s_last_flip_mx != s_mx || s_last_flip_my != s_my || s_last_flip_cur != s_cursor) {
        flip_output(true);
    }
}

void ipo_gui_request_initial_video(const char *url) {
    gui_set_str(&s_pending_initial_video,
                (url && *url) ? url : "https://www.youtube.com/watch?v=YE7VzlLtp-4");
}

int ipo_gui_standalone_video_player(const char *url) {
    ipo_gui_init();
    int slot = ensure_tab_slot();
    if (slot < 0) slot = 0;
    memset(&s_tabs[slot], 0, sizeof(s_tabs[slot]));
    s_tabs[slot].used = true;
    s_active_tab = slot;
    enter_video_in_tab(slot, url);
    while (!terminate_loop) {
        ipo_gui_tick();
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }
    terminate_loop = 0;
    ipo_gui_shutdown();
    return 0;
}

#endif
