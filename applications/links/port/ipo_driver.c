#include "cfg.h"

#ifdef GRDRV_IPO

#include "links.h"
#include "ipo_gui.h"
#include "ipo_crisp_fonts.h"

extern struct graphics_driver ipo_driver;

static unsigned char *ipo_init_driver(unsigned char *param, unsigned char *display) {
    (void)param;
    (void)display;

    if (gamma_bits == 2) {
        gamma_bits = 0;
    }
    ipo_gui_init();
    ipo_driver.x = ipo_gui_vp_w();
    ipo_driver.y = ipo_gui_vp_h();
    return NULL;
}

static struct graphics_device *ipo_init_device(void) {
    struct graphics_device *dev = mem_calloc(sizeof(struct graphics_device));
    dev->size.x1 = 0;
    dev->size.y1 = 0;
    dev->size.x2 = ipo_gui_vp_w();
    dev->size.y2 = ipo_gui_vp_h();
    set_clip_area(dev, &dev->size);
    ipo_gui_on_device_created(dev);
    return dev;
}

static void ipo_shutdown_device(struct graphics_device *dev) {
    if (!dev) return;
    ipo_gui_on_device_destroyed(dev);
    mem_free(dev);
}

static void ipo_shutdown_driver(void) {
    ipo_gui_shutdown();
}

static void ipo_emergency_shutdown(void) {
    ipo_gui_shutdown();
}

static void ipo_get_margin(int *left, int *right, int *top, int *bottom) {
    *left = 0;
    *right = 0;
    *top = 0;
    *bottom = 0;
}

static int ipo_set_margin(int left, int right, int top, int bottom) {
    (void)left;
    (void)right;
    (void)top;
    (void)bottom;
    return 0;
}

static int ipo_get_empty_bitmap(struct bitmap *dest) {
    dest->data = NULL;
    if (dest->x <= 0 || dest->y <= 0) {
        dest->skip = 0;
        dest->flags = NULL;
        return 0;
    }
    if ((size_t)dest->x > SIZE_MAX / 4u / (size_t)dest->y) {
        return -1;
    }
    dest->data = mem_alloc_mayfail((size_t)dest->x * (size_t)dest->y * 4u);
    if (!dest->data) {
        return -1;
    }
    dest->skip = (ssize_t)dest->x * 4;
    dest->flags = NULL;
    return 0;
}

static void ipo_register_bitmap(struct bitmap *bmp) {
    (void)bmp;
}

static void *ipo_prepare_strip(struct bitmap *bmp, int top, int lines) {
    (void)lines;
    if (!bmp || !bmp->data) return NULL;
    return ((unsigned char *)bmp->data) + bmp->skip * top;
}

static void ipo_commit_strip(struct bitmap *bmp, int top, int lines) {
    (void)bmp;
    (void)top;
    (void)lines;
}

static void ipo_unregister_bitmap(struct bitmap *bmp) {
    if (bmp && bmp->data) {
        mem_free(bmp->data);
        bmp->data = NULL;
    }
}

static void ipo_draw_bitmap(struct graphics_device *dev, struct bitmap *bmp, int x, int y) {
    CLIP_DRAW_BITMAP
    if (!bmp->data || !ipo_gui_is_viewport_live(dev)) return;

    int x1 = x;
    int y1 = y;
    int x2 = x + bmp->x;
    int y2 = y + bmp->y;
    if (x1 < dev->clip.x1) x1 = dev->clip.x1;
    if (y1 < dev->clip.y1) y1 = dev->clip.y1;
    if (x2 > dev->clip.x2) x2 = dev->clip.x2;
    if (y2 > dev->clip.y2) y2 = dev->clip.y2;

    int vp_x = ipo_gui_vp_x();
    int vp_y = ipo_gui_vp_y();
    int vp_w = ipo_gui_vp_w();
    int vp_h = ipo_gui_vp_h();
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > vp_w) x2 = vp_w;
    if (y2 > vp_h) y2 = vp_h;
    if (x1 >= x2 || y1 >= y2) return;

    uint32_t *fb32 = ipo_gui_fb32();
    const unsigned char *src_base = (const unsigned char *)bmp->data;
    int sx_off = x1 - x;
    int sy_off = y1 - y;
    int copy_w = x2 - x1;
    extern int ipo_drawing_text_bitmap;
    uint32_t px_flag = ipo_drawing_text_bitmap ? 0u : IPO_PX_DITHER;

    for (int ry = y1; ry < y2; ry++) {
        const uint32_t *src = (const uint32_t *)(src_base + (ssize_t)(ry - y1 + sy_off) * bmp->skip) + sx_off;
        uint32_t *dst = fb32 + (vp_y + ry) * IPO_GUI_W + (vp_x + x1);
        for (int cx = 0; cx < copy_w; cx++) {
            dst[cx] = (src[cx] & 0x00FFFFFFu) | px_flag;
        }
    }
    ipo_gui_mark_vp_dirty_rows(vp_y + y1, vp_y + y2);
}

static long ipo_get_color(int rgb) {
    return (long)((uint32_t)rgb & 0x00FFFFFFu);
}

static void ipo_fill_area(struct graphics_device *dev, int x1, int y1, int x2, int y2, long color) {
    CLIP_FILL_AREA
    if (!ipo_gui_is_viewport_live(dev)) return;

    int vp_x = ipo_gui_vp_x();
    int vp_y = ipo_gui_vp_y();
    int vp_w = ipo_gui_vp_w();
    int vp_h = ipo_gui_vp_h();
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > vp_w) x2 = vp_w;
    if (y2 > vp_h) y2 = vp_h;
    if (x1 >= x2 || y1 >= y2) return;

    uint32_t *fb32 = ipo_gui_fb32();
    uint32_t col = (uint32_t)color & 0x00FFFFFFu;
    int w = x2 - x1;
    for (int y = y1; y < y2; y++) {
        uint32_t *dst = fb32 + (vp_y + y) * IPO_GUI_W + (vp_x + x1);
        for (int i = 0; i < w; i++) {
            dst[i] = col;
        }
    }
    ipo_gui_mark_vp_dirty_rows(vp_y + y1, vp_y + y2);
}

static void ipo_draw_hline(struct graphics_device *dev, int x1, int y, int x2, long color) {
    CLIP_DRAW_HLINE
    if (!ipo_gui_is_viewport_live(dev)) return;

    int vp_x = ipo_gui_vp_x();
    int vp_y = ipo_gui_vp_y();
    int vp_w = ipo_gui_vp_w();
    int vp_h = ipo_gui_vp_h();
    if (y < 0 || y >= vp_h) return;
    if (x1 < 0) x1 = 0;
    if (x2 > vp_w) x2 = vp_w;
    if (x1 >= x2) return;

    uint32_t *fb32 = ipo_gui_fb32();
    uint32_t col = (uint32_t)color & 0x00FFFFFFu;
    uint32_t *dst = fb32 + (vp_y + y) * IPO_GUI_W + (vp_x + x1);
    int w = x2 - x1;
    for (int i = 0; i < w; i++) {
        dst[i] = col;
    }
    ipo_gui_mark_vp_dirty_rows(vp_y + y, vp_y + y + 1);
}

static void ipo_draw_vline(struct graphics_device *dev, int x, int y1, int y2, long color) {
    CLIP_DRAW_VLINE
    if (!ipo_gui_is_viewport_live(dev)) return;

    int vp_x = ipo_gui_vp_x();
    int vp_y = ipo_gui_vp_y();
    int vp_w = ipo_gui_vp_w();
    int vp_h = ipo_gui_vp_h();
    if (x < 0 || x >= vp_w) return;
    if (y1 < 0) y1 = 0;
    if (y2 > vp_h) y2 = vp_h;
    if (y1 >= y2) return;

    uint32_t *fb32 = ipo_gui_fb32();
    uint32_t col = (uint32_t)color & 0x00FFFFFFu;
    for (int y = y1; y < y2; y++) {
        fb32[(vp_y + y) * IPO_GUI_W + (vp_x + x)] = col;
    }
    ipo_gui_mark_vp_dirty_rows(vp_y + y1, vp_y + y2);
}

void ipo_driver_draw_overstrike(struct graphics_device *dev, int base_x, int y,
                                int base_w, int fidx, int height, uint32_t cp, uint32_t fg_rgb) {
    if (!ipo_gui_is_viewport_live(dev)) return;
    int vp_x = ipo_gui_vp_x();
    int vp_y = ipo_gui_vp_y();
    int vp_w = ipo_gui_vp_w();
    int vp_h = ipo_gui_vp_h();
    int cx1 = dev->clip.x1 < 0 ? 0 : dev->clip.x1;
    int cy1 = dev->clip.y1 < 0 ? 0 : dev->clip.y1;
    int cx2 = dev->clip.x2 > vp_w ? vp_w : dev->clip.x2;
    int cy2 = dev->clip.y2 > vp_h ? vp_h : dev->clip.y2;
    if (cx1 >= cx2 || cy1 >= cy2) return;
    uint32_t *fb32 = ipo_gui_fb32();
    ipo_font_draw_overstrike_rgb32(fb32, IPO_GUI_W,
                                   vp_x + cx1, vp_y + cy1, vp_x + cx2, vp_y + cy2,
                                   vp_x + base_x, base_w, vp_y + y, fidx, height, cp, fg_rgb);
    int r0 = vp_y + (y < cy1 ? cy1 : y);
    int r1 = vp_y + (y + height > cy2 ? cy2 : y + height);
    if (r1 > r0) ipo_gui_mark_vp_dirty_rows(r0, r1);
}

static int ipo_scroll(struct graphics_device *dev, struct rect_set **set, int scx, int scy) {
    (void)set;
    if (!ipo_gui_is_viewport_live(dev)) return 1;
    if (!scx && !scy) return 1;

    int x1 = dev->clip.x1;
    int y1 = dev->clip.y1;
    int x2 = dev->clip.x2;
    int y2 = dev->clip.y2;
    int vp_x = ipo_gui_vp_x();
    int vp_y = ipo_gui_vp_y();
    int vp_w = ipo_gui_vp_w();
    int vp_h = ipo_gui_vp_h();
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > vp_w) x2 = vp_w;
    if (y2 > vp_h) y2 = vp_h;
    if (x1 >= x2 || y1 >= y2) return 1;

    ipo_gui_on_scroll(vp_x + x1, vp_y + y1, vp_x + x2, vp_y + y2, scx, scy);
    return 1;
}

static void ipo_flush(struct graphics_device *dev) {
    (void)dev;
    ipo_gui_present_now();
}

static int ipo_block(struct graphics_device *dev) {
    (void)dev;
    return 0;
}

static int ipo_unblock(struct graphics_device *dev) {
    (void)dev;
    return 0;
}

static void ipo_set_title(struct graphics_device *dev, unsigned char *title) {
    ipo_gui_set_title(dev, (const char *)title);
}

struct graphics_driver ipo_driver = {
    cast_uchar "ipo",
    ipo_init_driver,
    ipo_init_device,
    ipo_shutdown_device,
    ipo_shutdown_driver,
    ipo_emergency_shutdown,
    NULL,
    NULL,
    NULL,
    ipo_get_margin,
    ipo_set_margin,
    ipo_get_empty_bitmap,
    ipo_register_bitmap,
    ipo_prepare_strip,
    ipo_commit_strip,
    ipo_unregister_bitmap,
    ipo_draw_bitmap,
    ipo_get_color,
    ipo_fill_area,
    ipo_draw_hline,
    ipo_draw_vline,
    ipo_scroll,
    NULL,
    ipo_flush,
    ipo_block,
    ipo_unblock,
    NULL,
    NULL,
    ipo_set_title,
    NULL,
    NULL,
    NULL,
    196,
    IPO_GUI_DEFAULT_W,
    IPO_GUI_DEFAULT_H - 94 - 24,
    GD_NEED_CODEPAGE | GD_UNICODE_KEYS,
    NULL
};

#endif
