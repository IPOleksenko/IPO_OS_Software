#ifndef LINKS_IPO_CRISP_FONTS_H
#define LINKS_IPO_CRISP_FONTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPO_FONT_REGULAR 0
#define IPO_FONT_BOLD    1
#define IPO_FONT_MONO    2

#define IPO_SHAPED_OVERSTRIKE 0x01u
#define IPO_SHAPED_RTL        0x02u
#define IPO_SHAPED_ZEROWIDTH  0x04u

typedef struct {
    uint32_t cp;
    uint8_t  flags;
} ipo_shaped_glyph_t;

int ipo_font_has_glyph(uint32_t cp);
int ipo_font_is_wide(uint32_t cp);
int ipo_font_is_combining(uint32_t cp);
int ipo_font_is_zero_width(uint32_t cp);
uint32_t ipo_font_compose_pair(uint32_t base, uint32_t mark);

/* Shape UTF-8 string into visual glyph run (NFD->NFC, Arabic contextual + Lam-Alef, RTL runs).
 * Returns total shaped length. Caller supplies stack buf out[out_cap]; if len > out_cap,
 * remaining glyphs are not written beyond out_cap (caller may retry with heap buffer). */
int ipo_font_shape_utf8(const char *utf8, ipo_shaped_glyph_t *out, int out_cap);

int ipo_font_measure_glyph(int style_idx, int height, uint32_t cp);
int ipo_font_text_width(int style_idx, int height, const char *utf8);
int ipo_font_draw_glyph_rgb32(uint32_t *dst, int dst_stride,
                              int clip_x1, int clip_y1, int clip_x2, int clip_y2,
                              int x, int y, int style_idx, int height,
                              uint32_t cp, uint32_t fg_rgb);
int ipo_font_draw_overstrike_rgb32(uint32_t *dst, int dst_stride,
                                   int clip_x1, int clip_y1, int clip_x2, int clip_y2,
                                   int base_x, int base_w, int y,
                                   int style_idx, int height,
                                   uint32_t cp, uint32_t fg_rgb);
int ipo_font_raster_cell_bgr0(uint32_t *out_bgr0, int out_w, int out_h,
                              int style_idx, int height, int italic,
                              uint32_t cp, uint32_t fg_rgb, uint32_t bg_rgb);

/* Crisp tofu replacement box when no glyph table has a codepoint */
int ipo_font_measure_fallback(int height);
int ipo_font_raster_fallback_bgr0(uint32_t *out_bgr0, int out_w, int out_h,
                                  int height, uint32_t fg_rgb, uint32_t bg_rgb);

#ifdef __cplusplus
}
#endif

#endif
