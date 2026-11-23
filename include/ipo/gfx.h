#ifndef IPO_PUBLIC_GFX_H
#define IPO_PUBLIC_GFX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <driver/color.h>
#include <wm.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPO_GFX_API_VERSION 0x00010000u

/* BGA operation subcodes for IPO_SYSCALL_GFX_BGA_OP */
#define IPO_BGA_OP_IS_AVAILABLE 0u
#define IPO_BGA_OP_IS_ENABLED   1u
#define IPO_BGA_OP_GET_WIDTH    2u
#define IPO_BGA_OP_GET_HEIGHT   3u
#define IPO_BGA_OP_GET_BPP      4u
#define IPO_BGA_OP_PROBE        5u
#define IPO_BGA_OP_SET_MODE     6u
#define IPO_BGA_OP_RESTORE_TEXT 7u
#define IPO_BGA_OP_GET_LFB_ADDR 8u
#define IPO_BGA_OP_GET_VRAM     9u

typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t hw_available;
    uint32_t hw_bga_available;
    uint32_t hw_bpp_mask;           /* bits: 0=8, 1=15, 2=16, 3=24, 4=32 */
    uint32_t active_hw_bpp;
    uint32_t active_hw_fmt_id;      /* ipo_color_format_id_t */
    uint32_t active_width;
    uint32_t active_height;
    uint32_t max_hw_width;
    uint32_t max_hw_height;
    uint64_t vram_bytes;
    uint32_t lfb_wc_enabled;
    uint32_t sw_pipeline_max_fmt_id;/* IPO_COLOR_FMT_RGBA64F */
    uint32_t sw_pipeline_max_bpp;   /* 256 (plus arbitrary extended/custom formats) */
    uint32_t supports_hdr_compositor;
    uint32_t supports_arbitrary_bpp;
} ipo_gfx_caps_t;

typedef struct {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t format_id;
    uint32_t bytes_per_row;
    uint8_t  hw_supported;
    uint8_t  lfb_wc;
    uint16_t reserved;
} ipo_gfx_mode_info_t;

typedef struct {
    uint32_t                  struct_size;
    uint32_t                  width;
    uint32_t                  height;
    uint32_t                  stride_bytes;
    uint32_t                  format_id;      /* ipo_color_format_id_t (including RGBA64F/32F/16F/ARGB8888/CUSTOM) */
    uint32_t                  tonemap_op;     /* ipo_tonemap_op_t */
    const void               *pixels;
    const ipo_pixel_format_t *custom_format;  /* Required only if format_id == IPO_COLOR_FMT_CUSTOM */
    double                    exposure;       /* <= 0 treated as 1.0 */
    uint32_t                  dst_x;
    uint32_t                  dst_y;
    uint32_t                  dst_w;          /* 0 = fit destination window/screen */
    uint32_t                  dst_h;
} ipo_gfx_surface_desc_t;

typedef struct {
    uint32_t struct_size;
    uint64_t frames_presented;
    uint64_t direct_blits;
    uint64_t wm_composites;
    uint64_t dirty_rect_updates;
    uint64_t pixels_converted;
    uint32_t active_hw_bpp;
    uint32_t active_render_fmt_id;
    uint32_t lfb_wc_active;
    uint32_t reserved;
} ipo_gfx_stats_t;

uint32_t ipo_gfx_api_version(void);
int ipo_gfx_get_caps(ipo_gfx_caps_t *out_caps);
int ipo_gfx_enum_modes(uint32_t index, ipo_gfx_mode_info_t *out_mode);
int ipo_gfx_set_mode(uint32_t width, uint32_t height, uint32_t bpp);
int ipo_gfx_ensure_hd_mode(uint32_t pref_w, uint32_t pref_h);
int ipo_gfx_get_mode(ipo_gfx_mode_info_t *out_mode);
int ipo_gfx_select_best_formats(const ipo_color_select_req_t *req,
                                ipo_pixel_format_t *out_render,
                                ipo_pixel_format_t *out_hw);
int ipo_gfx_present_window(wm_window_t *win, const ipo_gfx_surface_desc_t *surf);
int ipo_gfx_present_screen(const ipo_gfx_surface_desc_t *surf);
int ipo_gfx_get_stats(ipo_gfx_stats_t *out_stats);
int ipo_gfx_sync_palette_32(uint32_t *out_xrgb256);

/* Logical WM window rect -> physical screen pixel rect helper */
void ipo_gfx_wm_window_phys_rect(const wm_window_t *win,
                                 int *out_px, int *out_py, int *out_pw, int *out_ph);

#ifdef __cplusplus
}
#endif

#endif /* IPO_PUBLIC_GFX_H */
