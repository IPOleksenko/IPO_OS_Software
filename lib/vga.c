#include <vga.h>
#include <vga_gfx.h>
#include <driver/vbe_bga.h>
#include <syscall.h>
#include <stdlib.h>
#include <string.h>

static uint16_t s_vga_hw_width = 0;
static uint16_t s_vga_hw_height = 0;
static uint32_t s_vga_hw_addr = 0;
static bool s_vga_hw_probed = false;

static int  s_gfx_screen_width = 0;
static int  s_gfx_screen_height = 0;
static bool s_gfx_screen_probed = false;

void vga_get_text_mode_info(uint16_t *out_w, uint16_t *out_h, uint32_t *out_vram_addr) {
    uint16_t w = 0, h = 0;
    uint32_t addr = 0;
    if (ipo_vga_get_text_info(&w, &h, &addr) == 0 && w > 0 && h > 0 && addr != 0) {
        s_vga_hw_width = w;
        s_vga_hw_height = h;
        s_vga_hw_addr = addr;
        s_vga_hw_probed = true;
    }
    if (out_w) *out_w = s_vga_hw_width;
    if (out_h) *out_h = s_vga_hw_height;
    if (out_vram_addr) *out_vram_addr = s_vga_hw_addr;
}

uint16_t vga_get_width(void) {
    if (!s_vga_hw_probed) vga_get_text_mode_info(NULL, NULL, NULL);
    return s_vga_hw_width;
}

uint16_t vga_get_height(void) {
    if (!s_vga_hw_probed) vga_get_text_mode_info(NULL, NULL, NULL);
    return s_vga_hw_height;
}

volatile uint16_t *vga_get_memory(void) {
    if (!s_vga_hw_probed) vga_get_text_mode_info(NULL, NULL, NULL);
    return (volatile uint16_t *)(uintptr_t)s_vga_hw_addr;
}

void vga_set_mode_13h(void) {
    uint32_t arg = 1u;
    ipo_syscall(IPO_SYSCALL_VGA_SET_MODE, 1u, &arg);
    s_gfx_screen_probed = false;
    vga_gfx_probe_screen_size();
}

void vga_set_mode_text(void) {
    uint32_t arg = 0u;
    ipo_syscall(IPO_SYSCALL_VGA_SET_MODE, 1u, &arg);
    s_gfx_screen_probed = false;
    vga_gfx_probe_screen_size();
}

bool vga_is_graphics_mode(void) {
    return ipo_syscall(IPO_SYSCALL_VGA_GET_MODE, 0u, NULL) != 0;
}

void vga_set_cursor(uint16_t offset) {
    uint32_t arg = (uint32_t)offset;
    ipo_syscall(IPO_SYSCALL_VGA_SET_CURSOR, 1u, &arg);
}

uint16_t vga_get_cursor_position(void) {
    return (uint16_t)ipo_syscall(IPO_SYSCALL_VGA_GET_CURSOR, 0u, NULL);
}

void vga_show_cursor(void) {
    ipo_syscall(IPO_SYSCALL_VGA_SHOW_CURSOR, 0u, NULL);
}

void vga_hide_cursor(void) {
    ipo_syscall(IPO_SYSCALL_VGA_HIDE_CURSOR, 0u, NULL);
}

void vga_gfx_set_palette(uint8_t idx, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t args[4] = { (uint32_t)idx, (uint32_t)r, (uint32_t)g, (uint32_t)b };
    ipo_syscall(IPO_SYSCALL_VGA_SET_PALETTE, 4u, args);
}

void vga_set_overlay_flags(uint32_t flags) {
    uint32_t arg = flags;
    ipo_syscall(IPO_SYSCALL_VGA_SET_OVERLAY_FLAGS, 1u, &arg);
}

uint32_t vga_get_overlay_flags(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_VGA_GET_OVERLAY_FLAGS, 0u, NULL);
}

void vga_hide_close_button(void) {
    vga_set_overlay_flags(vga_get_overlay_flags() | VGA_OVERLAY_HIDE_BUTTON);
}

void vga_show_close_button(void) {
    vga_set_overlay_flags(vga_get_overlay_flags() & ~VGA_OVERLAY_HIDE_BUTTON);
}

void vga_hide_overlay_cursor(void) {
    vga_set_overlay_flags(vga_get_overlay_flags() | VGA_OVERLAY_HIDE_CURSOR);
}

void vga_show_overlay_cursor(void) {
    vga_set_overlay_flags(vga_get_overlay_flags() & ~VGA_OVERLAY_HIDE_CURSOR);
}

void vga_gfx_flip(const uint8_t *buf) {
    if (buf) {
        uint32_t args[2] = { 2u, (uint32_t)(uintptr_t)buf };
        ipo_syscall(IPO_SYSCALL_VGA_SET_MODE, 2u, args);
    }
}

void vga_gfx_flip_scaled(const uint8_t *buf) {
    if (buf) {
        uint32_t args[2] = { 3u, (uint32_t)(uintptr_t)buf };
        ipo_syscall(IPO_SYSCALL_VGA_SET_MODE, 2u, args);
    }
}

void vga_gfx_flip_scaled_buffer(const uint8_t *buf, int width, int height) {
    if (buf && width > 0 && height > 0) {
        uint32_t args[4] = {
            3u,
            (uint32_t)(uintptr_t)buf,
            (uint32_t)width,
            (uint32_t)height
        };
        ipo_syscall(IPO_SYSCALL_VGA_SET_MODE, 4u, args);
    }
}

bool vga_gfx_set_resolution(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (width > 65535 || height > 65535) return false;
    uint32_t args[3] = { 4u, (uint32_t)width, (uint32_t)height };
    bool ok = ipo_syscall(IPO_SYSCALL_VGA_SET_MODE, 3u, args) != 0;
    s_gfx_screen_probed = false;
    vga_gfx_probe_screen_size();
    return ok;
}

__attribute__((constructor))
void vga_gfx_probe_screen_size(void) {
    int32_t width = 0;
    int32_t height = 0;
    if (ipo_vga_get_screen_bounds(&width, &height) == 0 &&
        width > 0 && height > 0) {
        s_gfx_screen_width = (int)width;
        s_gfx_screen_height = (int)height;
        s_gfx_screen_probed = true;
    }
}

void vga_gfx_get_screen_bounds(int *out_w, int *out_h) {
    vga_gfx_probe_screen_size();
    if (out_w) *out_w = s_gfx_screen_width;
    if (out_h) *out_h = s_gfx_screen_height;
}

int vga_gfx_get_width(void) {
    if (!s_gfx_screen_probed) {
        vga_gfx_probe_screen_size();
    }
    return s_gfx_screen_width;
}

int vga_gfx_get_height(void) {
    if (!s_gfx_screen_probed) {
        vga_gfx_probe_screen_size();
    }
    return s_gfx_screen_height;
}

void vga_font_set_app_mode(bool is_app) {
    (void)is_app;
}

int vga_font_load_database(const char *path) {
    uint32_t args[1] = { (uint32_t)(uintptr_t)path };
    return ipo_syscall(IPO_SYSCALL_FONT_LOAD, 1u, args);
}

void vga_load_font(uint8_t start_code, uint32_t count, const uint8_t *glyphs) {
    uint32_t args[3] = { (uint32_t)start_code, count, (uint32_t)(uintptr_t)glyphs };
    ipo_syscall(IPO_SYSCALL_FONT_LOAD, 3u, args);
}

const char* vga_font_get_current_name(void) {
    static char *font_name = NULL;
    static size_t font_name_cap = 0u;
    size_t try_cap = (font_name_cap >= 64u) ? font_name_cap : 64u;
    while (try_cap <= 65536u) {
        if (!font_name || try_cap > font_name_cap) {
            char *nb = (char *)realloc(font_name, try_cap);
            if (!nb) return "Default";
            font_name = nb;
            font_name_cap = try_cap;
        }
        font_name[0] = '\0';
        uint32_t cnt = 0;
        uint32_t args[3] = { (uint32_t)(uintptr_t)font_name, (uint32_t)font_name_cap, (uint32_t)(uintptr_t)&cnt };
        if (ipo_syscall(IPO_SYSCALL_FONT_GET_INFO, 3u, args) != 0) {
            return "Default";
        }
        if (strlen(font_name) + 1u < font_name_cap) {
            return font_name[0] ? font_name : "Default";
        }
        try_cap *= 2u;
    }
    return (font_name && font_name[0]) ? font_name : "Default";
}

uint32_t vga_font_get_glyph_count(void) {
    uint32_t cnt = 256;
    uint32_t args[3] = { 0u, 0u, (uint32_t)(uintptr_t)&cnt };
    if (ipo_syscall(IPO_SYSCALL_FONT_GET_INFO, 3u, args) == 0) {
        return cnt;
    }
    return 256;
}

uint8_t utf8_to_vga_glyph(const char *str, size_t str_len, size_t *out_bytes) {
    if (!str || str_len == 0) {
        if (out_bytes) *out_bytes = 0;
        return ' ';
    }
    unsigned char b1 = (unsigned char)str[0];
    if (b1 < 0x80) {
        if (out_bytes) *out_bytes = 1;
        return b1;
    }

    uint32_t codepoint = 0;
    size_t bytes = 1;

    if ((b1 & 0xE0) == 0xC0) {
        bytes = 2;
        if (str_len >= 2) {
            unsigned char b2 = (unsigned char)str[1];
            codepoint = ((b1 & 0x1F) << 6) | (b2 & 0x3F);
        } else {
            codepoint = b1;
        }
    } else if ((b1 & 0xF0) == 0xE0) {
        bytes = 3;
        if (str_len >= 3) {
            unsigned char b2 = (unsigned char)str[1];
            unsigned char b3 = (unsigned char)str[2];
            codepoint = ((b1 & 0x0F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        } else {
            codepoint = b1;
        }
    } else if ((b1 & 0xF8) == 0xF0) {
        bytes = 4;
        if (str_len >= 4) {
            unsigned char b2 = (unsigned char)str[1];
            unsigned char b3 = (unsigned char)str[2];
            unsigned char b4 = (unsigned char)str[3];
            codepoint = ((b1 & 0x07) << 18) | ((b2 & 0x3F) << 12) | ((b3 & 0x3F) << 6) | (b4 & 0x3F);
        } else {
            codepoint = b1;
        }
    } else {
        bytes = 1;
        codepoint = b1;
    }

    if (out_bytes) *out_bytes = bytes;

    uint32_t bytes_consumed = (uint32_t)bytes;
    uint32_t args[3] = { (uint32_t)(uintptr_t)str, (uint32_t)str_len, (uint32_t)(uintptr_t)&bytes_consumed };
    int glyph = ipo_syscall(IPO_SYSCALL_VGA_GLYPH, 3u, args);
    if (glyph >= 0 && glyph <= 255) {
        if (out_bytes && bytes_consumed > 0) *out_bytes = bytes_consumed;
        return (uint8_t)glyph;
    }

    int slot = ipo_vga_glyph(codepoint);
    if (slot >= 0 && slot <= 255) {
        return (uint8_t)slot;
    }

    if (codepoint == 0x2014 || codepoint == 0x2013 || codepoint == 0x2015 || codepoint == 0x2212 ||
        codepoint == 0x2010 || codepoint == 0x2011 || codepoint == 0x2012) {
        return '-';
    }
    if (codepoint == 0x2026) return '.';
    if (codepoint == 0x00AB || codepoint == 0x00BB || codepoint == 0x201C || codepoint == 0x201D || codepoint == 0x201E) return '"';
    if (codepoint == 0x2022) return '*';

    return (codepoint < 0x80) ? (uint8_t)codepoint : '?';
}

const uint8_t* vga_font_get_cached_glyph(uint8_t char_code) {
    /* Standard 8x16 bitmap font fallback data */
    return vga_default_font_get_glyph(char_code);
}

/* VBE BGA userland stubs */
bool vbe_bga_is_available(void) { return false; }
int vbe_bga_get_bpp(void) { return 0; }
void *vbe_bga_get_framebuffer(void) { return NULL; }
bool vbe_bga_is_enabled(void) { return false; }
uint32_t vbe_bga_get_lfb_address(void) { return 0; }
bool vbe_bga_set_mode(int width, int height, int bpp) { (void)width; (void)height; (void)bpp; return false; }
void vbe_bga_restore_text_mode(void) {}
int vbe_bga_get_width(void) { return 0; }
int vbe_bga_get_height(void) { return 0; }
bool vbe_bga_probe(vbe_bga_info_t *out_info) { (void)out_info; return false; }

/* Userland 2D buffer and VRAM drawing primitives */

void vga_gfx_buf_put_pixel(uint8_t *buf, int x, int y, uint8_t color) {
    if (!buf || x < 0 || x >= VGA_GFX_WIDTH || y < 0 || y >= VGA_GFX_HEIGHT) return;
    ((volatile uint8_t *)buf)[(size_t)y * (size_t)VGA_GFX_WIDTH + (size_t)x] = color;
}

void vga_gfx_buf_clear(uint8_t *buf, uint8_t color) {
    if (!buf) return;
    memset(buf, color, VGA_GFX_SIZE);
}

void vga_gfx_buf_fill_rect(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    if (!buf || w <= 0 || h <= 0) return;
    int64_t x2_64 = (int64_t)x + (int64_t)w;
    int64_t y2_64 = (int64_t)y + (int64_t)h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    int x2 = (x2_64 > VGA_GFX_WIDTH) ? VGA_GFX_WIDTH : (int)x2_64;
    int y2 = (y2_64 > VGA_GFX_HEIGHT) ? VGA_GFX_HEIGHT : (int)y2_64;
    if (x2 <= x || y2 <= y) return;

    for (int cy = y; cy < y2; cy++) {
        memset(buf + ((size_t)cy * (size_t)VGA_GFX_WIDTH + (size_t)x), color, (size_t)(x2 - x));
    }
}

void vga_gfx_buf_draw_line(uint8_t *buf, int x0, int y0, int x1, int y1, uint8_t color) {
    if (!buf) return;
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    while (1) {
        vga_gfx_buf_put_pixel(buf, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void vga_gfx_buf_draw_rect(uint8_t *buf, int x, int y, int w, int h, uint8_t color) {
    if (!buf || w <= 0 || h <= 0) return;
    int x_end = (int)((int64_t)x + (int64_t)w - 1);
    int y_end = (int)((int64_t)y + (int64_t)h - 1);
    vga_gfx_buf_draw_line(buf, x, y, x_end, y, color);
    vga_gfx_buf_draw_line(buf, x, y_end, x_end, y_end, color);
    vga_gfx_buf_draw_line(buf, x, y, x, y_end, color);
    vga_gfx_buf_draw_line(buf, x_end, y, x_end, y_end, color);
}

void vga_gfx_buf_draw_circle(uint8_t *buf, int xc, int yc, int r, uint8_t color) {
    if (!buf || r < 0) return;
    int x = 0;
    int y = r;
    int d = 3 - 2 * r;

    while (y >= x) {
        vga_gfx_buf_put_pixel(buf, xc + x, yc + y, color);
        vga_gfx_buf_put_pixel(buf, xc - x, yc + y, color);
        vga_gfx_buf_put_pixel(buf, xc + x, yc - y, color);
        vga_gfx_buf_put_pixel(buf, xc - x, yc - y, color);
        vga_gfx_buf_put_pixel(buf, xc + y, yc + x, color);
        vga_gfx_buf_put_pixel(buf, xc - y, yc + x, color);
        vga_gfx_buf_put_pixel(buf, xc + y, yc - x, color);
        vga_gfx_buf_put_pixel(buf, xc - y, yc - x, color);

        if (d <= 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
        x++;
    }
}

void vga_gfx_buf_fill_circle(uint8_t *buf, int xc, int yc, int r, uint8_t color) {
    if (!buf || r < 0) return;
    int64_t r2 = (int64_t)r * (int64_t)r;
    for (int dy = -r; dy <= r; dy++) {
        int64_t dy2 = (int64_t)dy * (int64_t)dy;
        for (int dx = -r; dx <= r; dx++) {
            if ((int64_t)dx * (int64_t)dx + dy2 <= r2) {
                vga_gfx_buf_put_pixel(buf, xc + dx, yc + dy, color);
            }
        }
    }
}

void vga_gfx_put_pixel(int x, int y, uint8_t color) {
    vga_gfx_buf_put_pixel((uint8_t *)(uintptr_t)VGA_GFX_VRAM_ADDR, x, y, color);
}

uint8_t vga_gfx_get_pixel(int x, int y) {
    if (x < 0 || x >= VGA_GFX_WIDTH || y < 0 || y >= VGA_GFX_HEIGHT) return 0;
    volatile uint8_t *vram = (volatile uint8_t *)(uintptr_t)VGA_GFX_VRAM_ADDR;
    return vram[(size_t)y * (size_t)VGA_GFX_WIDTH + (size_t)x];
}

void vga_gfx_clear(uint8_t color) {
    vga_gfx_buf_clear((uint8_t *)(uintptr_t)VGA_GFX_VRAM_ADDR, color);
}

void vga_gfx_draw_line(int x0, int y0, int x1, int y1, uint8_t color) {
    vga_gfx_buf_draw_line((uint8_t *)(uintptr_t)VGA_GFX_VRAM_ADDR, x0, y0, x1, y1, color);
}

void vga_gfx_draw_rect(int x, int y, int w, int h, uint8_t color) {
    vga_gfx_buf_draw_rect((uint8_t *)(uintptr_t)VGA_GFX_VRAM_ADDR, x, y, w, h, color);
}

void vga_gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    vga_gfx_buf_fill_rect((uint8_t *)(uintptr_t)VGA_GFX_VRAM_ADDR, x, y, w, h, color);
}

void vga_gfx_draw_circle(int xc, int yc, int r, uint8_t color) {
    vga_gfx_buf_draw_circle((uint8_t *)(uintptr_t)VGA_GFX_VRAM_ADDR, xc, yc, r, color);
}

void vga_gfx_fill_circle(int xc, int yc, int r, uint8_t color) {
    vga_gfx_buf_fill_circle((uint8_t *)(uintptr_t)VGA_GFX_VRAM_ADDR, xc, yc, r, color);
}
