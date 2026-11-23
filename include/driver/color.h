#ifndef LIB_DRIVER_COLOR_H
#define LIB_DRIVER_COLOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Predefined color format IDs (arbitrary custom formats also supported with IPO_COLOR_FMT_CUSTOM) */
typedef enum {
    IPO_COLOR_FMT_UNKNOWN   = 0,
    IPO_COLOR_FMT_INDEX8    = 1,   /* 8 bpp indexed palette */
    IPO_COLOR_FMT_RGB555    = 2,   /* 15/16 bpp X1R5G5B5 */
    IPO_COLOR_FMT_RGB565    = 3,   /* 16 bpp R5G6B5 */
    IPO_COLOR_FMT_RGB888    = 4,   /* 24 bpp BGR/RGB */
    IPO_COLOR_FMT_RGB10A2   = 5,   /* 30/32 bpp A2B10G10R10 / R10G10B10A2 */
    IPO_COLOR_FMT_XRGB8888  = 6,   /* 32 bpp X8R8G8B8 (little-endian B,G,R,X) */
    IPO_COLOR_FMT_ARGB8888  = 7,   /* 32 bpp A8R8G8B8 (little-endian B,G,R,A) */
    IPO_COLOR_FMT_RGBA8888  = 8,   /* 32 bpp R8G8B8A8 */
    IPO_COLOR_FMT_RGB16     = 9,   /* 48 bpp R16G16B16 UNORM */
    IPO_COLOR_FMT_RGBA16    = 10,  /* 64 bpp R16G16B16A16 UNORM */
    IPO_COLOR_FMT_RGBA16F   = 11,  /* 64 bpp IEEE 754 binary16 */
    IPO_COLOR_FMT_RGB32F    = 12,  /* 96 bpp IEEE 754 binary32 */
    IPO_COLOR_FMT_RGBA32F   = 13,  /* 128 bpp IEEE 754 binary32 */
    IPO_COLOR_FMT_RGB64F    = 14,  /* 192 bpp IEEE 754 binary64 */
    IPO_COLOR_FMT_RGBA64F   = 15,  /* 256 bpp IEEE 754 binary64 (priority high-precision format) */
    IPO_COLOR_FMT_RGBA128EXT = 16, /* 512 bpp extended multi-word software float */
    IPO_COLOR_FMT_RGBA256EXT = 17, /* 1024 bpp extended multi-word software float */
    IPO_COLOR_FMT_CUSTOM    = 0x1000u
} ipo_color_format_id_t;

typedef enum {
    IPO_CH_ROLE_NONE  = 0,
    IPO_CH_ROLE_R     = 1,
    IPO_CH_ROLE_G     = 2,
    IPO_CH_ROLE_B     = 3,
    IPO_CH_ROLE_A     = 4,
    IPO_CH_ROLE_GRAY  = 5,
    IPO_CH_ROLE_INDEX = 6
} ipo_channel_role_t;

typedef enum {
    IPO_NUM_UNORM = 0,     /* Normalized unsigned [0, 1] */
    IPO_NUM_SNORM = 1,     /* Normalized signed [-1, 1] */
    IPO_NUM_UINT  = 2,     /* Unsigned integer */
    IPO_NUM_SINT  = 3,     /* Signed integer */
    IPO_NUM_F16   = 4,     /* IEEE 754 binary16 */
    IPO_NUM_F32   = 5,     /* IEEE 754 binary32 */
    IPO_NUM_F64   = 6,     /* IEEE 754 binary64 */
    IPO_NUM_F_EXT = 7      /* Arbitrary extended-width float (binary64 payload + zero extension) */
} ipo_num_type_t;

typedef enum {
    IPO_ORDER_CUSTOM = 0,
    IPO_ORDER_RGBA   = 1,
    IPO_ORDER_BGRA   = 2,
    IPO_ORDER_ARGB   = 3,
    IPO_ORDER_RGB    = 4,
    IPO_ORDER_BGR    = 5,
    IPO_ORDER_INDEX  = 6
} ipo_channel_order_t;

typedef enum {
    IPO_ALPHA_NONE          = 0,
    IPO_ALPHA_STRAIGHT      = 1,
    IPO_ALPHA_PREMULTIPLIED = 2
} ipo_alpha_mode_t;

typedef enum {
    IPO_CS_SRGB       = 0,
    IPO_CS_LINEAR_RGB = 1,
    IPO_CS_DISPLAY_P3 = 2,
    IPO_CS_REC2020    = 3,
    IPO_CS_SCRGB      = 4,
    IPO_CS_HDR10      = 5
} ipo_color_space_t;

typedef enum {
    IPO_TF_SRGB      = 0,
    IPO_TF_LINEAR    = 1,
    IPO_TF_GAMMA22   = 2,
    IPO_TF_PQ_ST2084 = 3,
    IPO_TF_HLG       = 4
} ipo_transfer_fn_t;

typedef enum {
    IPO_TONEMAP_NONE     = 0,
    IPO_TONEMAP_CLAMP    = 1,
    IPO_TONEMAP_REINHARD = 2,
    IPO_TONEMAP_ACES     = 3
} ipo_tonemap_op_t;

#define IPO_COLOR_CAP_HW_SCANOUT   (1u << 0)
#define IPO_COLOR_CAP_SW_RENDER    (1u << 1)
#define IPO_COLOR_CAP_SW_COMPOSITE (1u << 2)
#define IPO_COLOR_CAP_HDR          (1u << 3)
#define IPO_COLOR_CAP_ALPHA        (1u << 4)
#define IPO_COLOR_CAP_PALETTE      (1u << 5)
#define IPO_COLOR_CAP_FLOAT        (1u << 6)

typedef struct {
    uint32_t bit_offset; /* Bit offset inside pixel */
    uint32_t bit_width;  /* Width in bits (supports arbitrary widths: 1..9999999+) */
    uint8_t  role;       /* ipo_channel_role_t */
    uint8_t  num_type;   /* ipo_num_type_t */
    uint8_t  is_signed;  /* 0/1 */
    uint8_t  reserved;
} ipo_channel_desc_t;

typedef struct {
    uint32_t           struct_size;
    uint32_t           format_id;      /* ipo_color_format_id_t */
    uint32_t           bpp;            /* Total bits per pixel (arbitrary, no upper limit) */
    uint32_t           bytes_per_pixel;/* Storage stride in bytes ((bpp + 7) / 8) */
    uint32_t           num_channels;   /* Dynamic channel count (no fixed upper limit) */
    uint32_t           channels_cap;   /* Allocated capacity when channels_owned != 0 */
    uint8_t            channel_order;  /* ipo_channel_order_t */
    uint8_t            endianness;     /* 0 = little-endian, 1 = big-endian */
    uint8_t            alpha_mode;     /* ipo_alpha_mode_t */
    uint8_t            color_space;    /* ipo_color_space_t */
    uint8_t            transfer_fn;    /* ipo_transfer_fn_t */
    uint8_t            has_alpha;
    uint8_t            is_floating;
    uint8_t            channels_owned; /* 1 if channels array is dynamically heap-allocated */
    float              hdr_max_nits;   /* Peak luminance (e.g. 100.0 SDR, 1000..10000 HDR) */
    uint32_t           caps_flags;     /* IPO_COLOR_CAP_* */
    ipo_channel_desc_t *channels;      /* Dynamically allocated or static descriptor array */
} ipo_pixel_format_t;

/* Priority 256-bpp RGBA64F pixel (IEEE 754 binary64 per channel) */
typedef struct {
    double r;
    double g;
    double b;
    double a;
} ipo_rgba64f_t;

/* 128-bpp RGBA32F pixel */
typedef struct {
    float r;
    float g;
    float b;
    float a;
} ipo_rgba32f_t;

/* 64-bpp RGBA16F pixel (IEEE 754 binary16) */
typedef struct {
    uint16_t r;
    uint16_t g;
    uint16_t b;
    uint16_t a;
} ipo_rgba16f_t;

typedef struct {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t hw_bpp_mask;         /* Bitmask: bit0=8, bit1=15, bit2=16, bit3=24, bit4=32 */
    uint32_t hw_active_bpp;
    uint64_t available_vram_bytes;
    uint64_t available_ram_bytes;
    uint8_t  hw_has_bga;
    uint8_t  prefer_hdr;
    uint8_t  need_alpha;
    uint8_t  reserved;
} ipo_color_select_req_t;

void ipo_color_format_from_id(ipo_color_format_id_t id, ipo_pixel_format_t *out_fmt);
bool ipo_color_format_init_custom(ipo_pixel_format_t *out_fmt,
                                  uint32_t bpp, uint32_t num_channels,
                                  const ipo_channel_desc_t *channels,
                                  ipo_channel_order_t order,
                                  ipo_alpha_mode_t alpha_mode,
                                  ipo_color_space_t color_space,
                                  ipo_transfer_fn_t transfer_fn);
bool ipo_color_format_add_channel(ipo_pixel_format_t *fmt, const ipo_channel_desc_t *ch);
void ipo_color_format_free(ipo_pixel_format_t *fmt);
bool ipo_color_format_is_valid(const ipo_pixel_format_t *fmt);
void ipo_color_select_best_formats(const ipo_color_select_req_t *req,
                                   ipo_pixel_format_t *out_render_fmt,
                                   ipo_pixel_format_t *out_hw_fmt);

/* Half-float binary16 <-> float/double helpers */
uint16_t ipo_f64_to_f16(double v);
double   ipo_f16_to_f64(uint16_t h);

/* Transfer & gamut conversions */
double ipo_tf_to_linear(ipo_transfer_fn_t tf, double v, double max_nits);
double ipo_tf_from_linear(ipo_transfer_fn_t tf, double lin, double max_nits);
void   ipo_cs_to_linear_srgb(ipo_color_space_t src_cs, double *r, double *g, double *b);
void   ipo_cs_from_linear_srgb(ipo_color_space_t dst_cs, double *r, double *g, double *b);

/* Tonemapping in linear light */
ipo_rgba64f_t ipo_rgba64f_tonemap(ipo_rgba64f_t c, ipo_tonemap_op_t op, double exposure);

/* Linear-light premultiplied & straight alpha blending in RGBA64F */
ipo_rgba64f_t ipo_rgba64f_to_linear(ipo_rgba64f_t c, const ipo_pixel_format_t *fmt);
ipo_rgba64f_t ipo_rgba64f_from_linear(ipo_rgba64f_t lin, const ipo_pixel_format_t *fmt);
ipo_rgba64f_t ipo_rgba64f_blend_over(ipo_rgba64f_t dst_lin_premul, ipo_rgba64f_t src_lin_premul);

/* Generic single-pixel and scanline conversions across ANY valid formats (including RGBA64F/32F/16F/arbitrary bpp) */
ipo_rgba64f_t ipo_color_unpack_rgba64f(const ipo_pixel_format_t *fmt, const void *pixel_ptr,
                                       const uint32_t *palette_xrgb8888);
void          ipo_color_pack_rgba64f(const ipo_pixel_format_t *fmt, void *pixel_ptr, ipo_rgba64f_t val);

void ipo_color_convert_scanline(const ipo_pixel_format_t *src_fmt, const void *src_row,
                                const ipo_pixel_format_t *dst_fmt, void *dst_row,
                                uint32_t pixel_count, ipo_tonemap_op_t tonemap,
                                const uint32_t *palette_xrgb8888);

#endif /* LIB_DRIVER_COLOR_H */
