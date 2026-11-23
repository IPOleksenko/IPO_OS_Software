#ifndef IPO_HD_PRESENT_H
#define IPO_HD_PRESENT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vga_gfx.h>
#include <wm.h>
#include <ipo/gfx.h>

typedef struct {
    int scr_w, scr_h;
    int src_w, src_h;
    int pres_x, pres_y, pres_w, pres_h;
    int scale_k;       /* 1, 2, 4 for exact integer blit when >= 1, else 0 */
    bool hw_direct_fb; /* true when fullscreen BGA framebuffer exceeds WM virtual 320x200 */
    bool prefer_int_scale;
    bool last_fullscreen;
    uint8_t *hd_buf;
    int *x_lut;
    int x_lut_w;
    uint32_t *xrgb_buf;
    size_t xrgb_cap;
    int hw_bpp_cached;
    bool caps_cached;
    bool pal32_primed;
    int pal32_w, pal32_h;
    wm_window_t *pal32_win;
    bool pal32_direct;
    uint32_t pal32_hash;
} ipo_hd_view_t;

static inline void ipo_hd_free(ipo_hd_view_t *v) {
    if (!v) return;
    free(v->hd_buf);
    free(v->x_lut);
    free(v->xrgb_buf);
    v->hd_buf = NULL;
    v->x_lut = NULL;
    v->xrgb_buf = NULL;
    v->xrgb_cap = 0;
    v->x_lut_w = 0;
    v->pal32_primed = false;
}

static inline void ipo_hd_setup(ipo_hd_view_t *v, wm_window_t *ww,
                                int src_w, int src_h, bool prefer_int_scale) {
    if (!v || src_w <= 0 || src_h <= 0) return;

    int bga_w = 320, bga_h = 200;
    if (!v->caps_cached) {
        ipo_gfx_caps_t caps;
        if (ipo_gfx_get_caps(&caps) == 0 && caps.active_width >= 320u && caps.active_height >= 200u) {
            v->hw_bpp_cached = (int)caps.active_hw_bpp;
            v->caps_cached = true;
            bga_w = (int)caps.active_width;
            bga_h = (int)caps.active_height;
        } else {
            vga_gfx_probe_screen_size();
            vga_gfx_get_screen_bounds(&bga_w, &bga_h);
        }
    } else {
        vga_gfx_get_screen_bounds(&bga_w, &bga_h);
    }
    if (bga_w < 320) bga_w = 320;
    if (bga_h < 200) bga_h = 200;

    bool is_fs = (ww == NULL) ? true : ww->fullscreen;
    bool direct_hw = (ww == NULL) || (is_fs && ((bga_w > (int)ww->w) || (bga_h > (int)ww->h)));
    int sw = direct_hw ? bga_w : ((ww && ww->w > 0) ? (int)ww->w : bga_w);
    int sh = direct_hw ? bga_h : ((ww && ww->h > 0) ? (int)ww->h : bga_h);

    if (v->hd_buf && (v->scr_w != sw || v->scr_h != sh || v->hw_direct_fb != (ww != NULL && direct_hw))) {
        free(v->hd_buf);
        v->hd_buf = NULL;
    }
    v->hw_direct_fb = (ww != NULL && direct_hw);
    v->last_fullscreen = is_fs;
    v->prefer_int_scale = prefer_int_scale;
    v->scr_w = sw;
    v->scr_h = sh;
    v->src_w = src_w;
    v->src_h = src_h;

    if (v->hw_direct_fb && !v->hd_buf) {
        v->hd_buf = (uint8_t *)calloc((size_t)sw * (size_t)sh, 1u);
    } else if (v->hw_direct_fb && v->hd_buf) {
        memset(v->hd_buf, 0, (size_t)sw * (size_t)sh);
    }

    int dw = 0, dh = 0, k = 0;
    if (prefer_int_scale && sw >= src_w && sh >= src_h) {
        int kx = sw / src_w;
        int ky = sh / src_h;
        k = (kx < ky) ? kx : ky;
        if (k >= 4) k = 4;
        else if (k >= 2) k = 2;
        else if (k >= 1) k = 1;
        else k = 0;
    }
    if (k > 0) {
        dw = src_w * k;
        dh = src_h * k;
    } else {
        dw = sw;
        dh = (sw * src_h) / src_w;
        if (dh > sh) {
            dh = sh;
            dw = (sh * src_w) / src_h;
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    v->scale_k = k;
    v->pres_w = dw;
    v->pres_h = dh;
    v->pres_x = (sw - dw) / 2;
    v->pres_y = (sh - dh) / 2;

    if (v->x_lut_w != dw) {
        free(v->x_lut);
        v->x_lut = (int *)malloc((size_t)dw * sizeof(int));
        v->x_lut_w = dw;
    }
    if (v->x_lut) {
        for (int x = 0; x < dw; x++) {
            v->x_lut[x] = (x * src_w) / dw;
        }
    }
}

static inline void ipo_hd_blit(const ipo_hd_view_t *v, uint8_t *dst_fb, int fbw, int fbh,
                               int x0, int y0, int dw, int dh,
                               const uint8_t *src, int src_w, int src_h, int src_stride) {
    if (!dst_fb || !src || dw <= 0 || dh <= 0 || fbw <= 0 || fbh <= 0) return;

    if (y0 > 0) {
        int top = (y0 < fbh) ? y0 : fbh;
        memset(dst_fb, 0, (size_t)top * (size_t)fbw);
    }
    if (y0 + dh < fbh) {
        int bot_y = (y0 + dh > 0) ? (y0 + dh) : 0;
        memset(dst_fb + (size_t)bot_y * (size_t)fbw, 0, (size_t)(fbh - bot_y) * (size_t)fbw);
    }

    int cx1 = (x0 + dw > fbw) ? (fbw - x0) : dw;
    int cy1 = (y0 + dh > fbh) ? (fbh - y0) : dh;
    if (cx1 <= 0 || cy1 <= 0) return;

    /* Fast exact 1:1 copy */
    if (v && v->scale_k == 1 && dw == src_w && dh == src_h && cx1 == dw) {
        for (int y = 0; y < cy1; y++) {
            uint8_t *drow = dst_fb + (size_t)(y0 + y) * (size_t)fbw;
            if (x0 > 0) memset(drow, 0, (size_t)x0);
            if (x0 + dw < fbw) memset(drow + x0 + dw, 0, (size_t)(fbw - (x0 + dw)));
            memcpy(drow + x0, src + (size_t)y * (size_t)src_stride, (size_t)dw);
        }
        return;
    }

    /* Fast exact 2x2 integer pixel block copy */
    if (v && v->scale_k == 2 && dw == src_w * 2 && dh == src_h * 2 && cx1 == dw && cy1 == dh) {
        for (int sy = 0; sy < src_h; sy++) {
            const uint8_t *srow = src + (size_t)sy * (size_t)src_stride;
            uint8_t *d0 = dst_fb + (size_t)(y0 + sy * 2) * (size_t)fbw;
            uint8_t *d1 = d0 + fbw;
            if (x0 > 0) memset(d0, 0, (size_t)x0);
            if (x0 + dw < fbw) memset(d0 + x0 + dw, 0, (size_t)(fbw - (x0 + dw)));
            uint16_t *p0 = (uint16_t *)(d0 + x0);
            for (int sx = 0; sx < src_w; sx++) {
                uint8_t c = srow[sx];
                p0[sx] = (uint16_t)(c | (c << 8));
            }
            memcpy(d1, d0, (size_t)fbw);
        }
        return;
    }

    /* Fast exact 4x4 integer pixel block copy */
    if (v && v->scale_k == 4 && dw == src_w * 4 && dh == src_h * 4 && cx1 == dw && cy1 == dh) {
        for (int sy = 0; sy < src_h; sy++) {
            const uint8_t *srow = src + (size_t)sy * (size_t)src_stride;
            uint8_t *d0 = dst_fb + (size_t)(y0 + sy * 4) * (size_t)fbw;
            if (x0 > 0) memset(d0, 0, (size_t)x0);
            if (x0 + dw < fbw) memset(d0 + x0 + dw, 0, (size_t)(fbw - (x0 + dw)));
            uint32_t *p0 = (uint32_t *)(d0 + x0);
            for (int sx = 0; sx < src_w; sx++) {
                uint32_t c = srow[sx];
                p0[sx] = c * 0x01010101u;
            }
            memcpy(d0 + fbw, d0, (size_t)fbw);
            memcpy(d0 + fbw * 2, d0, (size_t)fbw);
            memcpy(d0 + fbw * 3, d0, (size_t)fbw);
        }
        return;
    }

    /* Crisp general nearest-neighbor scaling (zero blur) */
    int prev_sy = -1;
    uint8_t *prev_dst = NULL;
    for (int y = 0; y < cy1; y++) {
        uint8_t *drow = dst_fb + (size_t)(y0 + y) * (size_t)fbw;
        if (x0 > 0) memset(drow, 0, (size_t)x0);
        if (x0 + cx1 < fbw) memset(drow + x0 + cx1, 0, (size_t)(fbw - (x0 + cx1)));
        int sy = (y * src_h) / dh;
        uint8_t *dst = drow + x0;
        if (sy == prev_sy && prev_dst) {
            memcpy(dst, prev_dst, (size_t)cx1);
            continue;
        }
        const uint8_t *srow = src + (size_t)sy * (size_t)src_stride;
        if (v && v->x_lut && cx1 <= v->x_lut_w) {
            const int *xlut = v->x_lut;
            for (int x = 0; x < cx1; x++) dst[x] = srow[xlut[x]];
        } else {
            for (int x = 0; x < cx1; x++) dst[x] = srow[(x * src_w) / dw];
        }
        prev_sy = sy;
        prev_dst = dst;
    }
}

static inline void ipo_hd_present(ipo_hd_view_t *v, wm_window_t *ww, bool direct_gfx,
                                  const uint8_t *src, int src_w, int src_h, int src_stride) {
    if (!v || !src) return;
    bool cur_fs = (ww && wm_is_window_valid(ww)) ? ww->fullscreen : true;
    if (v->src_w != src_w || v->src_h != src_h || (ww && cur_fs != v->last_fullscreen)) {
        ipo_hd_setup(v, ww, src_w, src_h, v->prefer_int_scale);
        if (ww && wm_is_window_valid(ww) && ww->framebuf && ww->w > 0 && ww->h > 0) {
            memset(ww->framebuf, 0, (size_t)ww->w * (size_t)ww->h);
        }
    }

    if (ww && wm_is_window_valid(ww)) {
        if (v->hw_direct_fb && ww->fullscreen && v->hd_buf && wm_get_focused() == ww) {
            ipo_hd_blit(v, v->hd_buf, v->scr_w, v->scr_h,
                        v->pres_x, v->pres_y, v->pres_w, v->pres_h,
                        src, src_w, src_h, src_stride);
            vga_gfx_flip_scaled_buffer(v->hd_buf, v->scr_w, v->scr_h);
        } else if (ww->framebuf && ww->w > 0 && ww->h > 0) {
            int fbw = (int)ww->w, fbh = (int)ww->h;
            int dw = fbw, dh = (fbw * src_h) / src_w;
            if (dh > fbh) { dh = fbh; dw = (fbh * src_w) / src_h; }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            if (dw < fbw || dh < fbh) {
                /* Margins around (dw, dh) are cleared row-by-row inside ipo_hd_blit */
            }
            ipo_hd_blit(NULL, ww->framebuf, fbw, fbh,
                        (fbw - dw) / 2, (fbh - dh) / 2, dw, dh,
                        src, src_w, src_h, src_stride);
            wm_invalidate(ww);
            wm_compositor_tick();
        }
    } else if (direct_gfx) {
        if (!v->hd_buf) {
            v->hd_buf = (uint8_t *)calloc((size_t)v->scr_w * (size_t)v->scr_h, 1u);
        }
        if (v->hd_buf && v->scr_w >= src_w && v->scr_h >= src_h) {
            ipo_hd_blit(v, v->hd_buf, v->scr_w, v->scr_h,
                        v->pres_x, v->pres_y, v->pres_w, v->pres_h,
                        src, src_w, src_h, src_stride);
            vga_gfx_flip_scaled_buffer(v->hd_buf, v->scr_w, v->scr_h);
        } else {
            vga_gfx_flip_scaled_buffer(src, src_w, src_h);
        }
    }
}

static inline bool ipo_hd_present_pal32(ipo_hd_view_t *v, wm_window_t *ww, bool direct_gfx,
                                        const uint8_t *src, int src_w, int src_h, int src_stride,
                                        const uint32_t *pal_xrgb256) {
    if (!v || !src || !pal_xrgb256 || src_w <= 0 || src_h <= 0) return false;
    if (!v->caps_cached) {
        ipo_gfx_caps_t caps;
        if (ipo_gfx_get_caps(&caps) == 0) {
            v->hw_bpp_cached = (int)caps.active_hw_bpp;
            v->caps_cached = true;
        }
    }
    if (!v->caps_cached || v->hw_bpp_cached < 15) return false;

    size_t need = (size_t)src_w * (size_t)src_h;
    if (!v->xrgb_buf || v->xrgb_cap < need) {
        uint32_t *nb = (uint32_t *)realloc(v->xrgb_buf, need * sizeof(uint32_t));
        if (!nb) return false;
        v->xrgb_buf = nb;
        v->xrgb_cap = need;
        v->pal32_primed = false;
    }

    uint32_t pal_hash = 2166136261u;
    for (int i = 0; i < 256; i++) {
        pal_hash = (pal_hash ^ pal_xrgb256[i]) * 16777619u;
    }

    bool use_dirty = (v->pal32_primed &&
                      v->pal32_w == src_w && v->pal32_h == src_h &&
                      v->pal32_win == ww && v->pal32_direct == direct_gfx &&
                      v->pal32_hash == pal_hash &&
                      ww && wm_is_window_valid(ww));
    int min_x = src_w, min_y = src_h, max_x = -1, max_y = -1;
    uint32_t *dst = v->xrgb_buf;
    for (int y = 0; y < src_h; y++) {
        const uint8_t *sr = src + (size_t)y * (size_t)src_stride;
        uint32_t *dr = dst + (size_t)y * (size_t)src_w;
        if (use_dirty) {
            int row_min = -1, row_max = -1;
            for (int x = 0; x < src_w; x++) {
                uint32_t c = pal_xrgb256[sr[x]];
                if (dr[x] != c) {
                    dr[x] = c;
                    if (row_min < 0) row_min = x;
                    row_max = x;
                }
            }
            if (row_min >= 0) {
                if (row_min < min_x) min_x = row_min;
                if (row_max > max_x) max_x = row_max;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        } else {
            for (int x = 0; x < src_w; x++) dr[x] = pal_xrgb256[sr[x]];
        }
    }

    if (use_dirty && max_y < min_y) {
        if (ww && wm_is_window_valid(ww)) {
            wm_compositor_tick();
        }
        return true;
    }

    v->pal32_primed = true;
    v->pal32_w = src_w;
    v->pal32_h = src_h;
    v->pal32_win = ww;
    v->pal32_direct = direct_gfx;
    v->pal32_hash = pal_hash;

    ipo_gfx_surface_desc_t surf;
    memset(&surf, 0, sizeof(surf));
    surf.struct_size = (uint32_t)sizeof(surf);
    surf.width = (uint32_t)src_w;
    surf.height = (uint32_t)src_h;
    surf.stride_bytes = (uint32_t)src_w * (uint32_t)sizeof(uint32_t);
    surf.format_id = IPO_COLOR_FMT_XRGB8888;
    surf.tonemap_op = IPO_TONEMAP_NONE;
    surf.exposure = 1.0;
    surf.pixels = dst;
    if (use_dirty && max_x >= min_x && max_y >= min_y) {
        surf.dst_x = (uint32_t)min_x;
        surf.dst_y = (uint32_t)min_y;
        surf.dst_w = (uint32_t)(max_x - min_x + 1);
        surf.dst_h = (uint32_t)(max_y - min_y + 1);
    }

    if (ww && wm_is_window_valid(ww)) {
        return ipo_gfx_present_window(ww, &surf) == 0;
    } else if (direct_gfx) {
        return ipo_gfx_present_screen(&surf) == 0;
    }
    return false;
}

static inline bool ipo_hd_present_xrgb32(ipo_hd_view_t *v, wm_window_t *ww, bool direct_gfx,
                                         const uint32_t *src, int src_w, int src_h, int src_stride_px) {
    if (!v || !src || src_w <= 0 || src_h <= 0 || src_stride_px < src_w) return false;
    ipo_gfx_caps_t caps;
    if (ipo_gfx_get_caps(&caps) == 0) {
        v->hw_bpp_cached = (int)caps.active_hw_bpp;
        v->caps_cached = true;
    }
    if (!v->caps_cached || v->hw_bpp_cached < 15) return false;

    size_t need = (size_t)src_w * (size_t)src_h;
    if (!v->xrgb_buf || v->xrgb_cap < need) {
        uint32_t *nb = (uint32_t *)realloc(v->xrgb_buf, need * sizeof(uint32_t));
        if (!nb) return false;
        v->xrgb_buf = nb;
        v->xrgb_cap = need;
        v->pal32_primed = false;
    }

    bool use_dirty = (v->pal32_primed &&
                      v->pal32_w == src_w && v->pal32_h == src_h &&
                      v->pal32_win == ww && v->pal32_direct == direct_gfx &&
                      v->pal32_hash == 0x58524742u &&
                      ww && wm_is_window_valid(ww));
    int min_x = src_w, min_y = src_h, max_x = -1, max_y = -1;
    uint32_t *dst = v->xrgb_buf;
    for (int y = 0; y < src_h; y++) {
        const uint32_t *sr = src + (size_t)y * (size_t)src_stride_px;
        uint32_t *dr = dst + (size_t)y * (size_t)src_w;
        if (use_dirty) {
            int row_min = -1, row_max = -1;
            for (int x = 0; x < src_w; x++) {
                uint32_t c = sr[x] | 0xFF000000u;
                if (dr[x] != c) {
                    dr[x] = c;
                    if (row_min < 0) row_min = x;
                    row_max = x;
                }
            }
            if (row_min >= 0) {
                if (row_min < min_x) min_x = row_min;
                if (row_max > max_x) max_x = row_max;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        } else {
            for (int x = 0; x < src_w; x++) dr[x] = sr[x] | 0xFF000000u;
        }
    }

    if (use_dirty && max_y < min_y) {
        if (ww && wm_is_window_valid(ww)) {
            wm_compositor_tick();
        }
        return true;
    }

    v->pal32_primed = true;
    v->pal32_w = src_w;
    v->pal32_h = src_h;
    v->pal32_win = ww;
    v->pal32_direct = direct_gfx;
    v->pal32_hash = 0x58524742u;

    ipo_gfx_surface_desc_t surf;
    memset(&surf, 0, sizeof(surf));
    surf.struct_size = (uint32_t)sizeof(surf);
    surf.width = (uint32_t)src_w;
    surf.height = (uint32_t)src_h;
    surf.stride_bytes = (uint32_t)src_w * (uint32_t)sizeof(uint32_t);
    surf.format_id = IPO_COLOR_FMT_XRGB8888;
    surf.tonemap_op = IPO_TONEMAP_NONE;
    surf.exposure = 1.0;
    surf.pixels = dst;
    if (use_dirty && max_x >= min_x && max_y >= min_y) {
        surf.dst_x = (uint32_t)min_x;
        surf.dst_y = (uint32_t)min_y;
        surf.dst_w = (uint32_t)(max_x - min_x + 1);
        surf.dst_h = (uint32_t)(max_y - min_y + 1);
    }

    if (ww && wm_is_window_valid(ww)) {
        return ipo_gfx_present_window(ww, &surf) == 0;
    } else if (direct_gfx) {
        return ipo_gfx_present_screen(&surf) == 0;
    }
    return false;
}

#endif
