#ifndef IPO_CSS_H
#define IPO_CSS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ipo_css_sheet ipo_css_sheet_t;

enum {
    IPO_CSS_DISP_UNSET = 0,
    IPO_CSS_DISP_NONE,
    IPO_CSS_DISP_BLOCK,
    IPO_CSS_DISP_INLINE,
    IPO_CSS_DISP_INLINE_BLOCK,
    IPO_CSS_DISP_FLEX_ROW,
    IPO_CSS_DISP_FLEX_COL,
    IPO_CSS_DISP_LIST_ITEM
};

enum {
    IPO_CSS_ALIGN_UNSET = -1,
    IPO_CSS_ALIGN_LEFT = 0,
    IPO_CSS_ALIGN_CENTER = 1,
    IPO_CSS_ALIGN_RIGHT = 2,
    IPO_CSS_ALIGN_JUSTIFY = 3
};

enum {
    IPO_CSS_WS_UNSET = -1,
    IPO_CSS_WS_NORMAL = 0,
    IPO_CSS_WS_NOWRAP = 1,
    IPO_CSS_WS_PRE = 2
};

/* Computed CSS properties for one element */
typedef struct ipo_css_props {
    uint8_t has_color;
    uint32_t color;

    uint8_t has_bg;
    uint32_t bg;

    uint8_t has_border;
    uint8_t border_w[4];     /* top, right, bottom, left (px) */
    uint32_t border_color;   /* representative solid border color */

    uint8_t has_padding;
    int16_t padding[4];      /* top, right, bottom, left (px) */

    uint8_t has_margin;
    int16_t margin[4];       /* top, right, bottom, left (px) */
    uint8_t margin_auto_l;
    uint8_t margin_auto_r;

    uint8_t has_width;
    int16_t width_val;       /* >0 px, <0 percent (-1..-100) */
    uint8_t has_max_width;
    int16_t max_width_val;   /* >0 px, <0 percent */

    uint8_t has_font_size;
    int16_t font_px;         /* resolved CSS px (1rem = 16px) */

    int8_t font_weight;      /* -1 unset, 0 normal, 1 bold */
    int8_t font_style;       /* -1 unset, 0 normal, 1 italic */
    int8_t text_deco;        /* -1 unset, 0 none, 1 underline, 2 line-through */
    int8_t font_mono;        /* -1 unset, 0 prop, 1 monospace */
    int8_t text_align;       /* IPO_CSS_ALIGN_* */
    int8_t white_space;      /* IPO_CSS_WS_* */
    int8_t text_transform;   /* 0 none, 1 uppercase, 2 lowercase */
    int8_t display;          /* IPO_CSS_DISP_* */
    int8_t visibility_hidden;/* -1 unset, 0 visible, 1 hidden */
    int8_t list_style_none;  /* -1 unset, 0 disc/etc, 1 none */
} ipo_css_props_t;

/* Ancestor node view passed from Links html_stack */
typedef struct ipo_css_node {
    const char *tag;         /* lowercased tag name */
    const char *id;          /* id attribute (or NULL) */
    const char *classes;     /* whitespace-separated class list (or NULL) */
    const unsigned char *raw_attr; /* Links raw attribute pointer for optional [attr] checks */
} ipo_css_node_t;

/* Dynamic per-line box paint descriptor stored on struct g_object_line */
typedef struct ipo_css_box_layer {
    int16_t x1;
    int16_t x2;
    uint8_t has_bg;
    uint32_t bg_rgb;
    uint8_t b_left_w;
    uint8_t b_right_w;
    uint32_t border_rgb;
} ipo_css_box_layer_t;

typedef struct ipo_css_line_box {
    uint32_t n_layers;
    ipo_css_box_layer_t *layers;
} ipo_css_line_box_t;

/* Inline box styling passed via par_format / special_f */
typedef struct ipo_css_box_push_param {
    int16_t margin_top;
    int16_t margin_bottom;
    int16_t margin_left;
    int16_t margin_right;
    uint8_t margin_auto_l;
    uint8_t margin_auto_r;
    int16_t pad_top;
    int16_t pad_right;
    int16_t pad_bottom;
    int16_t pad_left;
    uint8_t border_w[4];
    uint32_t border_rgb;
    uint8_t has_bg;
    uint32_t bg_rgb;
    int16_t width_val;       /* >0 px, <0 % */
    int16_t max_width_val;   /* >0 px, <0 % */
    uint8_t is_root_bg;      /* applies to body/html root background */
} ipo_css_box_push_param_t;

ipo_css_sheet_t *ipo_css_create(int viewport_w);
void ipo_css_free(ipo_css_sheet_t *sheet);
void ipo_css_set_viewport(ipo_css_sheet_t *sheet, int viewport_w);
void ipo_css_add_stylesheet(ipo_css_sheet_t *sheet, const char *css, size_t len);

/* Match stylesheet + inline style="" for node stack depth 0..n_ancestors (ancestors[0] = current element, ancestors[1] = parent, ...) */
void ipo_css_compute(const ipo_css_sheet_t *sheet,
                     const ipo_css_node_t *ancestors,
                     int n_ancestors,
                     const char *inline_style,
                     int parent_font_px,
                     uint32_t parent_bg_rgb,
                     ipo_css_props_t *out);

/* Quick helper for HTML table cell bgcolor extraction */
int ipo_css_resolve_table_bg(const ipo_css_sheet_t *sheet,
                               const char *tag,
                               const char *id,
                               const char *classes,
                               const char *inline_style,
                               uint32_t parent_bg_rgb,
                               uint32_t *out_rgb);

/* Parse CSS color string (hex / rgb / rgba / hsl / hsla / named) */
int ipo_css_parse_color(const ipo_css_sheet_t *sheet, const char *val, uint32_t parent_bg, uint32_t *out_rgb, int *out_transparent);

/* Map CSS px (16px baseline) -> Links font size 1..7 */
int ipo_css_px_to_links_size(int px);

#ifdef __cplusplus
}
#endif

#endif /* IPO_CSS_H */
