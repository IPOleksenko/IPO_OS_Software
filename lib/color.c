#include <driver/color.h>
#include <stdlib.h>
#include <string.h>

/* Userland mirror of universal arbitrary-bpp / RGBA64F color engine */
static double s_srgb8_to_lin_lut[256];
static double s_srgb10_to_lin_lut[1024];
static uint8_t s_lin12_to_srgb8_lut[4096];
static uint16_t s_lin12_to_srgb10_lut[4096];
static bool s_color_luts_ready = false;

static inline uint64_t f64_to_bits(double v) {
    union { double d; uint64_t u; } x;
    x.d = v;
    return x.u;
}

static inline double bits_to_f64(uint64_t u) {
    union { double d; uint64_t u; } x;
    x.u = u;
    return x.d;
}

static inline uint32_t f32_to_bits(float v) {
    union { float f; uint32_t u; } x;
    x.f = v;
    return x.u;
}

static inline float bits_to_f32(uint32_t u) {
    union { float f; uint32_t u; } x;
    x.u = u;
    return x.f;
}

static double color_fabs(double x) { return (x < 0.0) ? -x : x; }
static double color_clamp01(double x) { return (x <= 0.0) ? 0.0 : ((x >= 1.0) ? 1.0 : x); }
static double color_floor(double x) {
    int64_t i = (int64_t)x;
    return ((double)i > x) ? (double)(i - 1) : (double)i;
}

static double color_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    uint64_t u = f64_to_bits(x);
    u = (u >> 1) + 0x1FF8000000000000ULL;
    double y = bits_to_f64(u);
    for (int i = 0; i < 6; i++) y = 0.5 * (y + x / y);
    return y;
}

static double color_exp(double x) {
    if (x < -709.0) return 0.0;
    if (x > 709.0) return 1e308;
    const double ln2 = 0.69314718055994530942;
    const double inv_ln2 = 1.44269504088896340736;
    int k = (int)color_floor(x * inv_ln2 + 0.5);
    double r = x - (double)k * ln2;
    double term = 1.0, sum = 1.0;
    for (int n = 1; n <= 13; n++) { term *= r / (double)n; sum += term; }
    int64_t exp_bits = (int64_t)k + 1023;
    if (exp_bits <= 0) return 0.0;
    if (exp_bits >= 2047) return 1e308;
    return sum * bits_to_f64((uint64_t)exp_bits << 52);
}

static double color_log(double x) {
    if (x <= 0.0) return -1e308;
    uint64_t bits = f64_to_bits(x);
    int exp_unbiased = (int)((bits >> 52) & 0x7FFULL) - 1023;
    if ((bits & 0x7FF0000000000000ULL) == 0ULL) {
        while ((bits & (1ULL << 52)) == 0ULL) { bits <<= 1; exp_unbiased--; }
        exp_unbiased++;
    }
    double m = bits_to_f64((bits & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL);
    if (m > 1.4142135623730950488) { m *= 0.5; exp_unbiased++; }
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t, p = t, sum = t;
    for (int k = 3; k <= 19; k += 2) { p *= t2; sum += p / (double)k; }
    return 2.0 * sum + (double)exp_unbiased * 0.69314718055994530942;
}

static double color_pow(double a, double b) {
    if (a <= 0.0) return (b == 0.0) ? 1.0 : 0.0;
    if (b == 1.0) return a;
    if (b == 2.0) return a * a;
    return color_exp(b * color_log(a));
}

static void ensure_color_luts(void) {
    if (s_color_luts_ready) return;
    for (int i = 0; i < 256; i++) {
        s_srgb8_to_lin_lut[i] = ipo_tf_to_linear(IPO_TF_SRGB, (double)i / 255.0, 100.0);
    }
    for (int i = 0; i < 1024; i++) {
        s_srgb10_to_lin_lut[i] = ipo_tf_to_linear(IPO_TF_SRGB, (double)i / 1023.0, 100.0);
    }
    for (int i = 0; i < 4096; i++) {
        double s = ipo_tf_from_linear(IPO_TF_SRGB, (double)i / 4095.0, 100.0);
        int q = (int)(s * 255.0 + 0.5);
        if (q < 0) q = 0; else if (q > 255) q = 255;
        s_lin12_to_srgb8_lut[i] = (uint8_t)q;
        int q10 = (int)(s * 1023.0 + 0.5);
        if (q10 < 0) q10 = 0; else if (q10 > 1023) q10 = 1023;
        s_lin12_to_srgb10_lut[i] = (uint16_t)q10;
    }
    s_color_luts_ready = true;
}

uint16_t ipo_f64_to_f16(double v) {
    uint64_t b = f64_to_bits(v);
    uint16_t sign = (uint16_t)((b >> 48) & 0x8000u);
    int exp = (int)((b >> 52) & 0x7FFu) - 1023;
    uint64_t mant = b & 0x000FFFFFFFFFFFFFULL;
    if (((b >> 52) & 0x7FFu) == 0x7FFu) return (mant != 0ULL) ? (sign | 0x7E00u) : (sign | 0x7C00u);
    if (exp > 15) return sign | 0x7C00u;
    if (exp < -24) return sign;
    if (exp < -14) {
        uint64_t sig = (1ULL << 52) | mant;
        return sign | ((uint16_t)(sig >> (42 + (-14 - exp))) & 0x03FFu);
    }
    uint16_t he = (uint16_t)((exp + 15) << 10);
    uint16_t hm = (uint16_t)((mant + (1ULL << 41)) >> 42);
    if (hm >= 0x0400u) { he += 0x0400u; hm = 0u; if (he >= 0x7C00u) return sign | 0x7C00u; }
    return sign | he | hm;
}

double ipo_f16_to_f64(uint16_t h) {
    uint64_t sign = ((uint64_t)(h & 0x8000u)) << 48;
    uint32_t exp = (h >> 10) & 0x1Fu, mant = h & 0x03FFu;
    if (exp == 0u) {
        if (mant == 0u) return bits_to_f64(sign);
        int e = -14;
        while ((mant & 0x0400u) == 0u) { mant <<= 1; e--; }
        mant &= 0x03FFu;
        return bits_to_f64(sign | ((uint64_t)(e + 1023) << 52) | ((uint64_t)mant << 42));
    }
    if (exp == 0x1Fu) return bits_to_f64(sign | (0x7FFULL << 52) | ((uint64_t)mant << 42));
    return bits_to_f64(sign | ((uint64_t)((int)exp - 15 + 1023) << 52) | ((uint64_t)mant << 42));
}

double ipo_tf_to_linear(ipo_transfer_fn_t tf, double v, double max_nits) {
    double sign = (v < 0.0) ? -1.0 : 1.0, av = color_fabs(v);
    switch (tf) {
        case IPO_TF_LINEAR: return v;
        case IPO_TF_GAMMA22: return sign * color_pow(av, 2.2);
        case IPO_TF_PQ_ST2084: {
            const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
            double vp = color_pow(color_clamp01(av), 1.0 / m2);
            double num = vp - c1;
            if (num <= 0.0) return 0.0;
            double den = c2 - c3 * vp;
            if (den <= 1e-12) den = 1e-12;
            return sign * color_pow(num / den, 1.0 / m1) * ((max_nits > 1.0) ? (10000.0 / max_nits) : 100.0);
        }
        case IPO_TF_HLG: {
            const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
            double x = color_clamp01(av);
            return sign * ((x <= 0.5) ? ((x * x) / 3.0) : ((color_exp((x - c) / a) + b) / 12.0));
        }
        case IPO_TF_SRGB:
        default:
            return sign * ((av <= 0.04045) ? (av / 12.92) : color_pow((av + 0.055) / 1.055, 2.4));
    }
}

double ipo_tf_from_linear(ipo_transfer_fn_t tf, double lin, double max_nits) {
    double sign = (lin < 0.0) ? -1.0 : 1.0, al = color_fabs(lin);
    switch (tf) {
        case IPO_TF_LINEAR: return lin;
        case IPO_TF_GAMMA22: return sign * color_pow(al, 1.0 / 2.2);
        case IPO_TF_PQ_ST2084: {
            const double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
            double ym = color_pow(color_clamp01(al * ((max_nits > 1.0) ? (max_nits / 10000.0) : 0.01)), m1);
            return sign * color_pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
        }
        case IPO_TF_HLG: {
            const double a = 0.17883277, b = 0.28466892, c = 0.55991073;
            double v = (al <= 1.0 / 12.0) ? color_sqrt(3.0 * al) : (a * color_log(12.0 * al - b) + c);
            return sign * color_clamp01(v);
        }
        case IPO_TF_SRGB:
        default:
            return sign * ((al <= 0.0031308) ? (al * 12.92) : (1.055 * color_pow(al, 1.0 / 2.4) - 0.055));
    }
}

void ipo_cs_to_linear_srgb(ipo_color_space_t src_cs, double *r, double *g, double *b) {
    double R = *r, G = *g, B = *b;
    if (src_cs == IPO_CS_DISPLAY_P3) {
        *r =  1.224940176 * R - 0.224940176 * G;
        *g = -0.042056955 * R + 1.042056955 * G;
        *b = -0.019637555 * R - 0.078636046 * G + 1.098273600 * B;
    } else if (src_cs == IPO_CS_REC2020 || src_cs == IPO_CS_HDR10) {
        *r =  1.660491002 * R - 0.587641139 * G - 0.072849863 * B;
        *g = -0.124550475 * R + 1.132899897 * G - 0.008349422 * B;
        *b = -0.018150763 * R - 0.100578898 * G + 1.118729661 * B;
    }
}

void ipo_cs_from_linear_srgb(ipo_color_space_t dst_cs, double *r, double *g, double *b) {
    double R = *r, G = *g, B = *b;
    if (dst_cs == IPO_CS_DISPLAY_P3) {
        *r = 0.822462173 * R + 0.177537827 * G;
        *g = 0.033194199 * R + 0.966805801 * G;
        *b = 0.017082631 * R + 0.072397441 * G + 0.910519928 * B;
    } else if (dst_cs == IPO_CS_REC2020 || dst_cs == IPO_CS_HDR10) {
        *r = 0.627403896 * R + 0.329283038 * G + 0.043313066 * B;
        *g = 0.069097289 * R + 0.919540395 * G + 0.011362316 * B;
        *b = 0.016391439 * R + 0.088013308 * G + 0.895595253 * B;
    }
}

static inline double aces_channel(double x) {
    if (x <= 0.0) return 0.0;
    return color_clamp01((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

ipo_rgba64f_t ipo_rgba64f_tonemap(ipo_rgba64f_t c, ipo_tonemap_op_t op, double exposure) {
    if (exposure != 1.0) { c.r *= exposure; c.g *= exposure; c.b *= exposure; }
    if (op == IPO_TONEMAP_ACES) { c.r = aces_channel(c.r); c.g = aces_channel(c.g); c.b = aces_channel(c.b); }
    else if (op == IPO_TONEMAP_REINHARD) {
        c.r = (c.r > 0.0) ? (c.r / (1.0 + c.r)) : 0.0;
        c.g = (c.g > 0.0) ? (c.g / (1.0 + c.g)) : 0.0;
        c.b = (c.b > 0.0) ? (c.b / (1.0 + c.b)) : 0.0;
    } else if (op == IPO_TONEMAP_CLAMP) {
        c.r = color_clamp01(c.r); c.g = color_clamp01(c.g); c.b = color_clamp01(c.b);
    }
    c.a = color_clamp01(c.a);
    return c;
}

ipo_rgba64f_t ipo_rgba64f_to_linear(ipo_rgba64f_t c, const ipo_pixel_format_t *fmt) {
    if (!fmt) return c;
    double max_nits = (fmt->hdr_max_nits > 0.0f) ? (double)fmt->hdr_max_nits : 100.0;
    if (fmt->alpha_mode == IPO_ALPHA_PREMULTIPLIED && c.a > 1e-12) {
        double inv_a = 1.0 / c.a; c.r *= inv_a; c.g *= inv_a; c.b *= inv_a;
    }
    c.r = ipo_tf_to_linear((ipo_transfer_fn_t)fmt->transfer_fn, c.r, max_nits);
    c.g = ipo_tf_to_linear((ipo_transfer_fn_t)fmt->transfer_fn, c.g, max_nits);
    c.b = ipo_tf_to_linear((ipo_transfer_fn_t)fmt->transfer_fn, c.b, max_nits);
    ipo_cs_to_linear_srgb((ipo_color_space_t)fmt->color_space, &c.r, &c.g, &c.b);
    return c;
}

ipo_rgba64f_t ipo_rgba64f_from_linear(ipo_rgba64f_t lin, const ipo_pixel_format_t *fmt) {
    if (!fmt) return lin;
    double max_nits = (fmt->hdr_max_nits > 0.0f) ? (double)fmt->hdr_max_nits : 100.0;
    ipo_cs_from_linear_srgb((ipo_color_space_t)fmt->color_space, &lin.r, &lin.g, &lin.b);
    lin.r = ipo_tf_from_linear((ipo_transfer_fn_t)fmt->transfer_fn, lin.r, max_nits);
    lin.g = ipo_tf_from_linear((ipo_transfer_fn_t)fmt->transfer_fn, lin.g, max_nits);
    lin.b = ipo_tf_from_linear((ipo_transfer_fn_t)fmt->transfer_fn, lin.b, max_nits);
    if (fmt->alpha_mode == IPO_ALPHA_PREMULTIPLIED) {
        double a = color_clamp01(lin.a); lin.r *= a; lin.g *= a; lin.b *= a;
    }
    return lin;
}

ipo_rgba64f_t ipo_rgba64f_blend_over(ipo_rgba64f_t dst_lin_premul, ipo_rgba64f_t src_lin_premul) {
    double inv_a = 1.0 - color_clamp01(src_lin_premul.a);
    ipo_rgba64f_t out = {
        src_lin_premul.r + dst_lin_premul.r * inv_a,
        src_lin_premul.g + dst_lin_premul.g * inv_a,
        src_lin_premul.b + dst_lin_premul.b * inv_a,
        color_clamp01(src_lin_premul.a + dst_lin_premul.a * inv_a)
    };
    return out;
}

static const ipo_channel_desc_t k_ch_index8[] = {
    { 0u, 8u, IPO_CH_ROLE_INDEX, IPO_NUM_UINT, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_rgb555[] = {
    { 10u, 5u, IPO_CH_ROLE_R, IPO_NUM_UNORM, 0u, 0u },
    {  5u, 5u, IPO_CH_ROLE_G, IPO_NUM_UNORM, 0u, 0u },
    {  0u, 5u, IPO_CH_ROLE_B, IPO_NUM_UNORM, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_rgb565[] = {
    { 11u, 5u, IPO_CH_ROLE_R, IPO_NUM_UNORM, 0u, 0u },
    {  5u, 6u, IPO_CH_ROLE_G, IPO_NUM_UNORM, 0u, 0u },
    {  0u, 5u, IPO_CH_ROLE_B, IPO_NUM_UNORM, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_rgb888[] = {
    { 16u, 8u, IPO_CH_ROLE_R, IPO_NUM_UNORM, 0u, 0u },
    {  8u, 8u, IPO_CH_ROLE_G, IPO_NUM_UNORM, 0u, 0u },
    {  0u, 8u, IPO_CH_ROLE_B, IPO_NUM_UNORM, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_rgb10a2[] = {
    {  0u, 10u, IPO_CH_ROLE_R, IPO_NUM_UNORM, 0u, 0u },
    { 10u, 10u, IPO_CH_ROLE_G, IPO_NUM_UNORM, 0u, 0u },
    { 20u, 10u, IPO_CH_ROLE_B, IPO_NUM_UNORM, 0u, 0u },
    { 30u,  2u, IPO_CH_ROLE_A, IPO_NUM_UNORM, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_xrgb8888[] = {
    { 16u, 8u, IPO_CH_ROLE_R, IPO_NUM_UNORM, 0u, 0u },
    {  8u, 8u, IPO_CH_ROLE_G, IPO_NUM_UNORM, 0u, 0u },
    {  0u, 8u, IPO_CH_ROLE_B, IPO_NUM_UNORM, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_argb8888[] = {
    { 16u, 8u, IPO_CH_ROLE_R, IPO_NUM_UNORM, 0u, 0u },
    {  8u, 8u, IPO_CH_ROLE_G, IPO_NUM_UNORM, 0u, 0u },
    {  0u, 8u, IPO_CH_ROLE_B, IPO_NUM_UNORM, 0u, 0u },
    { 24u, 8u, IPO_CH_ROLE_A, IPO_NUM_UNORM, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_rgba8888[] = {
    {  0u, 8u, IPO_CH_ROLE_R, IPO_NUM_UNORM, 0u, 0u },
    {  8u, 8u, IPO_CH_ROLE_G, IPO_NUM_UNORM, 0u, 0u },
    { 16u, 8u, IPO_CH_ROLE_B, IPO_NUM_UNORM, 0u, 0u },
    { 24u, 8u, IPO_CH_ROLE_A, IPO_NUM_UNORM, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_rgb16[] = {
    {  0u, 16u, IPO_CH_ROLE_R, IPO_NUM_UNORM, 0u, 0u },
    { 16u, 16u, IPO_CH_ROLE_G, IPO_NUM_UNORM, 0u, 0u },
    { 32u, 16u, IPO_CH_ROLE_B, IPO_NUM_UNORM, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_rgba16[] = {
    {  0u, 16u, IPO_CH_ROLE_R, IPO_NUM_UNORM, 0u, 0u },
    { 16u, 16u, IPO_CH_ROLE_G, IPO_NUM_UNORM, 0u, 0u },
    { 32u, 16u, IPO_CH_ROLE_B, IPO_NUM_UNORM, 0u, 0u },
    { 48u, 16u, IPO_CH_ROLE_A, IPO_NUM_UNORM, 0u, 0u },
};
static const ipo_channel_desc_t k_ch_rgba16f[] = {
    {  0u, 16u, IPO_CH_ROLE_R, IPO_NUM_F16, 1u, 0u },
    { 16u, 16u, IPO_CH_ROLE_G, IPO_NUM_F16, 1u, 0u },
    { 32u, 16u, IPO_CH_ROLE_B, IPO_NUM_F16, 1u, 0u },
    { 48u, 16u, IPO_CH_ROLE_A, IPO_NUM_F16, 1u, 0u },
};
static const ipo_channel_desc_t k_ch_rgb32f[] = {
    {  0u, 32u, IPO_CH_ROLE_R, IPO_NUM_F32, 1u, 0u },
    { 32u, 32u, IPO_CH_ROLE_G, IPO_NUM_F32, 1u, 0u },
    { 64u, 32u, IPO_CH_ROLE_B, IPO_NUM_F32, 1u, 0u },
};
static const ipo_channel_desc_t k_ch_rgba32f[] = {
    {  0u, 32u, IPO_CH_ROLE_R, IPO_NUM_F32, 1u, 0u },
    { 32u, 32u, IPO_CH_ROLE_G, IPO_NUM_F32, 1u, 0u },
    { 64u, 32u, IPO_CH_ROLE_B, IPO_NUM_F32, 1u, 0u },
    { 96u, 32u, IPO_CH_ROLE_A, IPO_NUM_F32, 1u, 0u },
};
static const ipo_channel_desc_t k_ch_rgb64f[] = {
    {   0u, 64u, IPO_CH_ROLE_R, IPO_NUM_F64, 1u, 0u },
    {  64u, 64u, IPO_CH_ROLE_G, IPO_NUM_F64, 1u, 0u },
    { 128u, 64u, IPO_CH_ROLE_B, IPO_NUM_F64, 1u, 0u },
};
static const ipo_channel_desc_t k_ch_rgba64f[] = {
    {   0u, 64u, IPO_CH_ROLE_R, IPO_NUM_F64, 1u, 0u },
    {  64u, 64u, IPO_CH_ROLE_G, IPO_NUM_F64, 1u, 0u },
    { 128u, 64u, IPO_CH_ROLE_B, IPO_NUM_F64, 1u, 0u },
    { 192u, 64u, IPO_CH_ROLE_A, IPO_NUM_F64, 1u, 0u },
};
static const ipo_channel_desc_t k_ch_rgba128ext[] = {
    {   0u, 128u, IPO_CH_ROLE_R, IPO_NUM_F_EXT, 1u, 0u },
    { 128u, 128u, IPO_CH_ROLE_G, IPO_NUM_F_EXT, 1u, 0u },
    { 256u, 128u, IPO_CH_ROLE_B, IPO_NUM_F_EXT, 1u, 0u },
    { 384u, 128u, IPO_CH_ROLE_A, IPO_NUM_F_EXT, 1u, 0u },
};
static const ipo_channel_desc_t k_ch_rgba256ext[] = {
    {   0u, 256u, IPO_CH_ROLE_R, IPO_NUM_F_EXT, 1u, 0u },
    { 256u, 256u, IPO_CH_ROLE_G, IPO_NUM_F_EXT, 1u, 0u },
    { 512u, 256u, IPO_CH_ROLE_B, IPO_NUM_F_EXT, 1u, 0u },
    { 768u, 256u, IPO_CH_ROLE_A, IPO_NUM_F_EXT, 1u, 0u },
};

void ipo_color_format_from_id(ipo_color_format_id_t id, ipo_pixel_format_t *out_fmt) {
    if (!out_fmt) return;
    memset(out_fmt, 0, sizeof(*out_fmt));
    out_fmt->struct_size = sizeof(*out_fmt);
    out_fmt->format_id = (uint32_t)id;
    out_fmt->hdr_max_nits = 100.0f;
    out_fmt->channels = NULL;
    out_fmt->channels_owned = 0u;
    out_fmt->channels_cap = 0u;
    switch (id) {
        case IPO_COLOR_FMT_INDEX8:
            out_fmt->bpp = 8u; out_fmt->bytes_per_pixel = 1u; out_fmt->num_channels = 1u;
            out_fmt->channel_order = IPO_ORDER_INDEX;
            out_fmt->caps_flags = IPO_COLOR_CAP_HW_SCANOUT | IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_PALETTE;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_index8;
            break;
        case IPO_COLOR_FMT_RGB555:
            out_fmt->bpp = 15u; out_fmt->bytes_per_pixel = 2u; out_fmt->num_channels = 3u;
            out_fmt->channel_order = IPO_ORDER_BGR;
            out_fmt->caps_flags = IPO_COLOR_CAP_HW_SCANOUT | IPO_COLOR_CAP_SW_RENDER;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgb555;
            break;
        case IPO_COLOR_FMT_RGB565:
            out_fmt->bpp = 16u; out_fmt->bytes_per_pixel = 2u; out_fmt->num_channels = 3u;
            out_fmt->channel_order = IPO_ORDER_BGR;
            out_fmt->caps_flags = IPO_COLOR_CAP_HW_SCANOUT | IPO_COLOR_CAP_SW_RENDER;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgb565;
            break;
        case IPO_COLOR_FMT_RGB888:
            out_fmt->bpp = 24u; out_fmt->bytes_per_pixel = 3u; out_fmt->num_channels = 3u;
            out_fmt->channel_order = IPO_ORDER_BGR;
            out_fmt->caps_flags = IPO_COLOR_CAP_HW_SCANOUT | IPO_COLOR_CAP_SW_RENDER;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgb888;
            break;
        case IPO_COLOR_FMT_RGB10A2:
            out_fmt->bpp = 32u; out_fmt->bytes_per_pixel = 4u; out_fmt->num_channels = 4u;
            out_fmt->channel_order = IPO_ORDER_RGBA; out_fmt->alpha_mode = IPO_ALPHA_STRAIGHT; out_fmt->has_alpha = 1u;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_ALPHA | IPO_COLOR_CAP_HDR;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgb10a2;
            break;
        case IPO_COLOR_FMT_XRGB8888:
            out_fmt->bpp = 32u; out_fmt->bytes_per_pixel = 4u; out_fmt->num_channels = 3u;
            out_fmt->channel_order = IPO_ORDER_BGR;
            out_fmt->caps_flags = IPO_COLOR_CAP_HW_SCANOUT | IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_SW_COMPOSITE;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_xrgb8888;
            break;
        case IPO_COLOR_FMT_ARGB8888:
            out_fmt->bpp = 32u; out_fmt->bytes_per_pixel = 4u; out_fmt->num_channels = 4u;
            out_fmt->channel_order = IPO_ORDER_BGRA; out_fmt->alpha_mode = IPO_ALPHA_STRAIGHT; out_fmt->has_alpha = 1u;
            out_fmt->caps_flags = IPO_COLOR_CAP_HW_SCANOUT | IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_SW_COMPOSITE | IPO_COLOR_CAP_ALPHA;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_argb8888;
            break;
        case IPO_COLOR_FMT_RGBA8888:
            out_fmt->bpp = 32u; out_fmt->bytes_per_pixel = 4u; out_fmt->num_channels = 4u;
            out_fmt->channel_order = IPO_ORDER_RGBA; out_fmt->alpha_mode = IPO_ALPHA_STRAIGHT; out_fmt->has_alpha = 1u;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_SW_COMPOSITE | IPO_COLOR_CAP_ALPHA;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgba8888;
            break;
        case IPO_COLOR_FMT_RGB16:
            out_fmt->bpp = 48u; out_fmt->bytes_per_pixel = 6u; out_fmt->num_channels = 3u;
            out_fmt->channel_order = IPO_ORDER_RGB; out_fmt->color_space = IPO_CS_LINEAR_RGB; out_fmt->transfer_fn = IPO_TF_LINEAR;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_SW_COMPOSITE;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgb16;
            break;
        case IPO_COLOR_FMT_RGBA16:
            out_fmt->bpp = 64u; out_fmt->bytes_per_pixel = 8u; out_fmt->num_channels = 4u;
            out_fmt->channel_order = IPO_ORDER_RGBA; out_fmt->alpha_mode = IPO_ALPHA_PREMULTIPLIED; out_fmt->has_alpha = 1u;
            out_fmt->color_space = IPO_CS_LINEAR_RGB; out_fmt->transfer_fn = IPO_TF_LINEAR;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_SW_COMPOSITE | IPO_COLOR_CAP_ALPHA;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgba16;
            break;
        case IPO_COLOR_FMT_RGBA16F:
            out_fmt->bpp = 64u; out_fmt->bytes_per_pixel = 8u; out_fmt->num_channels = 4u;
            out_fmt->channel_order = IPO_ORDER_RGBA; out_fmt->alpha_mode = IPO_ALPHA_PREMULTIPLIED; out_fmt->has_alpha = 1u;
            out_fmt->is_floating = 1u; out_fmt->hdr_max_nits = 1000.0f;
            out_fmt->color_space = IPO_CS_LINEAR_RGB; out_fmt->transfer_fn = IPO_TF_LINEAR;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_SW_COMPOSITE | IPO_COLOR_CAP_ALPHA | IPO_COLOR_CAP_HDR | IPO_COLOR_CAP_FLOAT;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgba16f;
            break;
        case IPO_COLOR_FMT_RGB32F:
            out_fmt->bpp = 96u; out_fmt->bytes_per_pixel = 12u; out_fmt->num_channels = 3u;
            out_fmt->channel_order = IPO_ORDER_RGB; out_fmt->is_floating = 1u; out_fmt->hdr_max_nits = 4000.0f;
            out_fmt->color_space = IPO_CS_LINEAR_RGB; out_fmt->transfer_fn = IPO_TF_LINEAR;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_HDR | IPO_COLOR_CAP_FLOAT;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgb32f;
            break;
        case IPO_COLOR_FMT_RGBA32F:
            out_fmt->bpp = 128u; out_fmt->bytes_per_pixel = 16u; out_fmt->num_channels = 4u;
            out_fmt->channel_order = IPO_ORDER_RGBA; out_fmt->alpha_mode = IPO_ALPHA_PREMULTIPLIED; out_fmt->has_alpha = 1u;
            out_fmt->is_floating = 1u; out_fmt->hdr_max_nits = 4000.0f;
            out_fmt->color_space = IPO_CS_LINEAR_RGB; out_fmt->transfer_fn = IPO_TF_LINEAR;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_SW_COMPOSITE | IPO_COLOR_CAP_ALPHA | IPO_COLOR_CAP_HDR | IPO_COLOR_CAP_FLOAT;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgba32f;
            break;
        case IPO_COLOR_FMT_RGB64F:
            out_fmt->bpp = 192u; out_fmt->bytes_per_pixel = 24u; out_fmt->num_channels = 3u;
            out_fmt->channel_order = IPO_ORDER_RGB; out_fmt->is_floating = 1u; out_fmt->hdr_max_nits = 10000.0f;
            out_fmt->color_space = IPO_CS_LINEAR_RGB; out_fmt->transfer_fn = IPO_TF_LINEAR;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_HDR | IPO_COLOR_CAP_FLOAT;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgb64f;
            break;
        case IPO_COLOR_FMT_RGBA64F:
            out_fmt->bpp = 256u; out_fmt->bytes_per_pixel = 32u; out_fmt->num_channels = 4u;
            out_fmt->channel_order = IPO_ORDER_RGBA; out_fmt->alpha_mode = IPO_ALPHA_PREMULTIPLIED; out_fmt->has_alpha = 1u;
            out_fmt->is_floating = 1u; out_fmt->hdr_max_nits = 10000.0f;
            out_fmt->color_space = IPO_CS_LINEAR_RGB; out_fmt->transfer_fn = IPO_TF_LINEAR;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_SW_COMPOSITE | IPO_COLOR_CAP_ALPHA | IPO_COLOR_CAP_HDR | IPO_COLOR_CAP_FLOAT;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgba64f;
            break;
        case IPO_COLOR_FMT_RGBA128EXT:
            out_fmt->bpp = 512u; out_fmt->bytes_per_pixel = 64u; out_fmt->num_channels = 4u;
            out_fmt->channel_order = IPO_ORDER_RGBA; out_fmt->alpha_mode = IPO_ALPHA_PREMULTIPLIED; out_fmt->has_alpha = 1u;
            out_fmt->is_floating = 1u; out_fmt->hdr_max_nits = 10000.0f;
            out_fmt->color_space = IPO_CS_LINEAR_RGB; out_fmt->transfer_fn = IPO_TF_LINEAR;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_ALPHA | IPO_COLOR_CAP_HDR | IPO_COLOR_CAP_FLOAT;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgba128ext;
            break;
        case IPO_COLOR_FMT_RGBA256EXT:
            out_fmt->bpp = 1024u; out_fmt->bytes_per_pixel = 128u; out_fmt->num_channels = 4u;
            out_fmt->channel_order = IPO_ORDER_RGBA; out_fmt->alpha_mode = IPO_ALPHA_PREMULTIPLIED; out_fmt->has_alpha = 1u;
            out_fmt->is_floating = 1u; out_fmt->hdr_max_nits = 10000.0f;
            out_fmt->color_space = IPO_CS_LINEAR_RGB; out_fmt->transfer_fn = IPO_TF_LINEAR;
            out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_ALPHA | IPO_COLOR_CAP_HDR | IPO_COLOR_CAP_FLOAT;
            out_fmt->channels = (ipo_channel_desc_t *)(uintptr_t)k_ch_rgba256ext;
            break;
        default: break;
    }
}

void ipo_color_format_free(ipo_pixel_format_t *fmt) {
    if (!fmt) return;
    if (fmt->channels_owned && fmt->channels) {
        free(fmt->channels);
    }
    fmt->channels = NULL;
    fmt->channels_owned = 0u;
    fmt->channels_cap = 0u;
    fmt->num_channels = 0u;
}

bool ipo_color_format_add_channel(ipo_pixel_format_t *fmt, const ipo_channel_desc_t *ch) {
    if (!fmt || !ch || ch->bit_width == 0u ||
        (uint64_t)ch->bit_offset + (uint64_t)ch->bit_width > (uint64_t)fmt->bpp) {
        return false;
    }
    if (!fmt->channels_owned) {
        uint32_t init_cap = (fmt->num_channels + 1u > 4u) ? (fmt->num_channels + 4u) : 4u;
        if ((size_t)init_cap > (size_t)-1 / sizeof(ipo_channel_desc_t)) return false;
        ipo_channel_desc_t *nb = (ipo_channel_desc_t *)malloc((size_t)init_cap * sizeof(ipo_channel_desc_t));
        if (!nb) return false;
        if (fmt->channels && fmt->num_channels > 0u) {
            memcpy(nb, fmt->channels, (size_t)fmt->num_channels * sizeof(ipo_channel_desc_t));
        }
        fmt->channels = nb;
        fmt->channels_cap = init_cap;
        fmt->channels_owned = 1u;
    } else if (fmt->num_channels >= fmt->channels_cap) {
        uint32_t new_cap = fmt->channels_cap ? (fmt->channels_cap * 2u) : 4u;
        if (new_cap <= fmt->num_channels) new_cap = fmt->num_channels + 1u;
        if ((size_t)new_cap > (size_t)-1 / sizeof(ipo_channel_desc_t)) return false;
        ipo_channel_desc_t *nb = (ipo_channel_desc_t *)realloc(fmt->channels, (size_t)new_cap * sizeof(ipo_channel_desc_t));
        if (!nb) return false;
        fmt->channels = nb;
        fmt->channels_cap = new_cap;
    }
    fmt->channels[fmt->num_channels++] = *ch;
    if (ch->role == IPO_CH_ROLE_A) { fmt->has_alpha = 1u; fmt->caps_flags |= IPO_COLOR_CAP_ALPHA; }
    if (ch->num_type >= IPO_NUM_F16) { fmt->is_floating = 1u; fmt->caps_flags |= IPO_COLOR_CAP_FLOAT | IPO_COLOR_CAP_HDR; }
    return true;
}

bool ipo_color_format_init_custom(ipo_pixel_format_t *out_fmt,
                                  uint32_t bpp, uint32_t num_channels,
                                  const ipo_channel_desc_t *channels,
                                  ipo_channel_order_t order,
                                  ipo_alpha_mode_t alpha_mode,
                                  ipo_color_space_t color_space,
                                  ipo_transfer_fn_t transfer_fn) {
    if (!out_fmt || bpp == 0u || (num_channels > 0u && !channels)) {
        return false;
    }
    for (uint32_t i = 0u; i < num_channels; i++) {
        if (channels[i].bit_width == 0u ||
            (uint64_t)channels[i].bit_offset + (uint64_t)channels[i].bit_width > (uint64_t)bpp) {
            return false;
        }
    }
    ipo_channel_desc_t *ch_buf = NULL;
    if (num_channels > 0u) {
        if ((size_t)num_channels > (size_t)-1 / sizeof(ipo_channel_desc_t)) return false;
        ch_buf = (ipo_channel_desc_t *)malloc((size_t)num_channels * sizeof(ipo_channel_desc_t));
        if (!ch_buf) return false;
    }

    memset(out_fmt, 0, sizeof(*out_fmt));
    out_fmt->struct_size = sizeof(*out_fmt);
    out_fmt->format_id = IPO_COLOR_FMT_CUSTOM;
    out_fmt->bpp = bpp;
    out_fmt->bytes_per_pixel = (uint32_t)(((uint64_t)bpp + 7ULL) >> 3);
    out_fmt->num_channels = num_channels;
    out_fmt->channels_cap = num_channels;
    out_fmt->channels_owned = (ch_buf != NULL) ? 1u : 0u;
    out_fmt->channels = ch_buf;
    out_fmt->channel_order = (uint8_t)order;
    out_fmt->alpha_mode = (uint8_t)alpha_mode;
    out_fmt->color_space = (uint8_t)color_space;
    out_fmt->transfer_fn = (uint8_t)transfer_fn;
    out_fmt->hdr_max_nits = (transfer_fn == IPO_TF_PQ_ST2084 || color_space == IPO_CS_HDR10) ? 1000.0f : 100.0f;
    out_fmt->caps_flags = IPO_COLOR_CAP_SW_RENDER | IPO_COLOR_CAP_SW_COMPOSITE;
    for (uint32_t i = 0u; i < num_channels; i++) {
        out_fmt->channels[i] = channels[i];
        if (channels[i].role == IPO_CH_ROLE_A) { out_fmt->has_alpha = 1u; out_fmt->caps_flags |= IPO_COLOR_CAP_ALPHA; }
        if (channels[i].num_type >= IPO_NUM_F16) { out_fmt->is_floating = 1u; out_fmt->caps_flags |= IPO_COLOR_CAP_FLOAT | IPO_COLOR_CAP_HDR; }
    }
    return true;
}

bool ipo_color_format_is_valid(const ipo_pixel_format_t *fmt) {
    if (!fmt || fmt->struct_size < sizeof(ipo_pixel_format_t) || fmt->bpp == 0u ||
        fmt->num_channels == 0u || !fmt->channels) return false;
    for (uint32_t i = 0u; i < fmt->num_channels; i++) {
        if (fmt->channels[i].bit_width == 0u ||
            (uint64_t)fmt->channels[i].bit_offset + (uint64_t)fmt->channels[i].bit_width > (uint64_t)fmt->bpp) return false;
    }
    return true;
}

void ipo_color_select_best_formats(const ipo_color_select_req_t *req,
                                   ipo_pixel_format_t *out_render_fmt,
                                   ipo_pixel_format_t *out_hw_fmt) {
    if (!req) return;
    ipo_color_format_id_t hw_id = IPO_COLOR_FMT_INDEX8;
    if (req->hw_has_bga) {
        if (req->hw_bpp_mask & (1u << 4)) hw_id = IPO_COLOR_FMT_XRGB8888;
        else if (req->hw_bpp_mask & (1u << 3)) hw_id = IPO_COLOR_FMT_RGB888;
        else if (req->hw_bpp_mask & (1u << 2)) hw_id = IPO_COLOR_FMT_RGB565;
        else if (req->hw_bpp_mask & (1u << 1)) hw_id = IPO_COLOR_FMT_RGB555;
    }
    if (out_hw_fmt) ipo_color_format_from_id(hw_id, out_hw_fmt);
    uint64_t px = (uint64_t)(req->width ? req->width : 800u) * (uint64_t)(req->height ? req->height : 600u);
    uint64_t ram = req->available_ram_bytes;
    ipo_color_format_id_t ren_id = IPO_COLOR_FMT_RGBA64F;
    if (ram > 0ULL && (px * 32ULL) > (ram / 8ULL)) {
        if ((px * 16ULL) <= (ram / 8ULL)) ren_id = IPO_COLOR_FMT_RGBA32F;
        else if ((px * 8ULL) <= (ram / 8ULL)) ren_id = IPO_COLOR_FMT_RGBA16F;
        else ren_id = IPO_COLOR_FMT_ARGB8888;
    }
    if (out_render_fmt) ipo_color_format_from_id(ren_id, out_render_fmt);
}

static uint64_t read_bits_le(const uint8_t *p, uint32_t bit_off, uint32_t width) {
    if (width == 0u) return 0ULL;
    uint32_t byte_off = bit_off >> 3, shift = bit_off & 7u, need_bytes = (shift + width + 7u) >> 3;
    if (need_bytes > 8u) need_bytes = 8u;
    uint64_t acc = 0ULL;
    for (uint32_t i = 0u; i < need_bytes; i++) acc |= ((uint64_t)p[byte_off + i]) << (i * 8u);
    acc >>= shift;
    if (width < 64u) acc &= ((1ULL << width) - 1ULL);
    return acc;
}

static void write_bits_le(uint8_t *p, uint32_t bit_off, uint32_t width, uint64_t val) {
    if (width == 0u) return;
    if (width < 64u) val &= ((1ULL << width) - 1ULL);
    uint32_t byte_off = bit_off >> 3, shift = bit_off & 7u, bit_pos = 0u;
    while (bit_pos < width) {
        uint32_t cur_bit = shift + bit_pos, b_idx = byte_off + (cur_bit >> 3), b_off = cur_bit & 7u;
        uint32_t take = 8u - b_off;
        if (take > (width - bit_pos)) take = width - bit_pos;
        uint8_t mask = (uint8_t)(((1u << take) - 1u) << b_off);
        p[b_idx] = (uint8_t)((p[b_idx] & ~mask) | (((val >> bit_pos) << b_off) & mask));
        bit_pos += take;
    }
}

ipo_rgba64f_t ipo_color_unpack_rgba64f(const ipo_pixel_format_t *fmt, const void *pixel_ptr,
                                       const uint32_t *palette_xrgb8888) {
    ipo_rgba64f_t out = { 0.0, 0.0, 0.0, 1.0 };
    if (!fmt || !pixel_ptr) return out;
    ensure_color_luts();
    const uint8_t *p = (const uint8_t *)pixel_ptr;
    bool linear_srgb = (fmt->transfer_fn == IPO_TF_LINEAR && fmt->color_space == IPO_CS_LINEAR_RGB);
    bool srgb_std = (fmt->transfer_fn == IPO_TF_SRGB && fmt->color_space == IPO_CS_SRGB &&
                     fmt->alpha_mode != IPO_ALPHA_PREMULTIPLIED);
    if (linear_srgb) {
        switch (fmt->format_id) {
            case IPO_COLOR_FMT_RGBA64F:
                out = *(const ipo_rgba64f_t *)pixel_ptr;
                if (fmt->alpha_mode == IPO_ALPHA_PREMULTIPLIED && out.a > 1e-12 && out.a < 1.0) {
                    double inv_a = 1.0 / out.a;
                    out.r *= inv_a; out.g *= inv_a; out.b *= inv_a;
                }
                return out;
            case IPO_COLOR_FMT_RGB64F: {
                const double *d = (const double *)pixel_ptr;
                out.r = d[0]; out.g = d[1]; out.b = d[2]; return out;
            }
            case IPO_COLOR_FMT_RGBA32F: {
                const ipo_rgba32f_t *f = (const ipo_rgba32f_t *)pixel_ptr;
                out.r = f->r; out.g = f->g; out.b = f->b; out.a = f->a;
                if (fmt->alpha_mode == IPO_ALPHA_PREMULTIPLIED && out.a > 1e-12 && out.a < 1.0) {
                    double inv_a = 1.0 / out.a;
                    out.r *= inv_a; out.g *= inv_a; out.b *= inv_a;
                }
                return out;
            }
            case IPO_COLOR_FMT_RGB32F: {
                const float *f = (const float *)pixel_ptr;
                out.r = f[0]; out.g = f[1]; out.b = f[2]; return out;
            }
            case IPO_COLOR_FMT_RGBA16F: {
                const ipo_rgba16f_t *h = (const ipo_rgba16f_t *)pixel_ptr;
                out.r = ipo_f16_to_f64(h->r); out.g = ipo_f16_to_f64(h->g);
                out.b = ipo_f16_to_f64(h->b); out.a = ipo_f16_to_f64(h->a);
                if (fmt->alpha_mode == IPO_ALPHA_PREMULTIPLIED && out.a > 1e-12 && out.a < 1.0) {
                    double inv_a = 1.0 / out.a;
                    out.r *= inv_a; out.g *= inv_a; out.b *= inv_a;
                }
                return out;
            }
            default: break;
        }
    }
    if (srgb_std) {
        switch (fmt->format_id) {
            case IPO_COLOR_FMT_XRGB8888:
            case IPO_COLOR_FMT_ARGB8888: {
                uint32_t px = *(const uint32_t *)pixel_ptr;
                out.r = s_srgb8_to_lin_lut[(px >> 16) & 0xFFu];
                out.g = s_srgb8_to_lin_lut[(px >> 8) & 0xFFu];
                out.b = s_srgb8_to_lin_lut[px & 0xFFu];
                out.a = (fmt->format_id == IPO_COLOR_FMT_ARGB8888) ? ((double)((px >> 24) & 0xFFu) / 255.0) : 1.0;
                return out;
            }
            case IPO_COLOR_FMT_RGBA8888: {
                uint32_t px = *(const uint32_t *)pixel_ptr;
                out.r = s_srgb8_to_lin_lut[px & 0xFFu];
                out.g = s_srgb8_to_lin_lut[(px >> 8) & 0xFFu];
                out.b = s_srgb8_to_lin_lut[(px >> 16) & 0xFFu];
                out.a = (double)((px >> 24) & 0xFFu) / 255.0;
                return out;
            }
            case IPO_COLOR_FMT_RGB10A2: {
                uint32_t px = *(const uint32_t *)pixel_ptr;
                out.r = s_srgb10_to_lin_lut[px & 0x3FFu];
                out.g = s_srgb10_to_lin_lut[(px >> 10) & 0x3FFu];
                out.b = s_srgb10_to_lin_lut[(px >> 20) & 0x3FFu];
                out.a = (double)((px >> 30) & 0x3u) / 3.0;
                return out;
            }
            case IPO_COLOR_FMT_RGB888:
                out.r = s_srgb8_to_lin_lut[p[2]];
                out.g = s_srgb8_to_lin_lut[p[1]];
                out.b = s_srgb8_to_lin_lut[p[0]];
                return out;
            case IPO_COLOR_FMT_RGB565: {
                uint16_t px = *(const uint16_t *)pixel_ptr;
                uint32_t r5 = (px >> 11) & 0x1Fu, g6 = (px >> 5) & 0x3Fu, b5 = px & 0x1Fu;
                out.r = s_srgb8_to_lin_lut[(r5 << 3) | (r5 >> 2)];
                out.g = s_srgb8_to_lin_lut[(g6 << 2) | (g6 >> 4)];
                out.b = s_srgb8_to_lin_lut[(b5 << 3) | (b5 >> 2)];
                return out;
            }
            case IPO_COLOR_FMT_RGB555: {
                uint16_t px = *(const uint16_t *)pixel_ptr;
                uint32_t r5 = (px >> 10) & 0x1Fu, g5 = (px >> 5) & 0x1Fu, b5 = px & 0x1Fu;
                out.r = s_srgb8_to_lin_lut[(r5 << 3) | (r5 >> 2)];
                out.g = s_srgb8_to_lin_lut[(g5 << 3) | (g5 >> 2)];
                out.b = s_srgb8_to_lin_lut[(b5 << 3) | (b5 >> 2)];
                return out;
            }
            case IPO_COLOR_FMT_INDEX8: {
                uint32_t c = palette_xrgb8888 ? palette_xrgb8888[*p] : (((uint32_t)(*p) * 0x010101u));
                out.r = s_srgb8_to_lin_lut[(c >> 16) & 0xFFu];
                out.g = s_srgb8_to_lin_lut[(c >> 8) & 0xFFu];
                out.b = s_srgb8_to_lin_lut[c & 0xFFu];
                return out;
            }
            default: break;
        }
    }
    for (uint32_t i = 0u; i < fmt->num_channels; i++) {
        const ipo_channel_desc_t *ch = &fmt->channels[i];
        double val = 0.0;
        if (ch->num_type == IPO_NUM_F64 || ch->num_type == IPO_NUM_F_EXT) val = bits_to_f64(read_bits_le(p, ch->bit_offset, 64u));
        else if (ch->num_type == IPO_NUM_F32) val = (double)bits_to_f32((uint32_t)read_bits_le(p, ch->bit_offset, 32u));
        else if (ch->num_type == IPO_NUM_F16) val = ipo_f16_to_f64((uint16_t)read_bits_le(p, ch->bit_offset, 16u));
        else {
            uint32_t w = (ch->bit_width > 53u) ? 53u : ch->bit_width;
            uint32_t boff = (ch->bit_width > 53u) ? (ch->bit_offset + (ch->bit_width - 53u)) : ch->bit_offset;
            uint64_t raw = read_bits_le(p, boff, w);
            uint64_t maxv = (w >= 64u) ? UINT64_MAX : ((1ULL << w) - 1ULL);
            val = (double)raw / (double)(maxv ? maxv : 1ULL);
        }
        if (ch->role == IPO_CH_ROLE_R) out.r = val;
        else if (ch->role == IPO_CH_ROLE_G) out.g = val;
        else if (ch->role == IPO_CH_ROLE_B) out.b = val;
        else if (ch->role == IPO_CH_ROLE_A) out.a = val;
        else if (ch->role == IPO_CH_ROLE_GRAY) out.r = out.g = out.b = val;
    }
    return ipo_rgba64f_to_linear(out, fmt);
}

void ipo_color_pack_rgba64f(const ipo_pixel_format_t *fmt, void *pixel_ptr, ipo_rgba64f_t val) {
    if (!fmt || !pixel_ptr) return;
    ensure_color_luts();
    bool linear_srgb = (fmt->transfer_fn == IPO_TF_LINEAR && fmt->color_space == IPO_CS_LINEAR_RGB);
    bool srgb_std = (fmt->transfer_fn == IPO_TF_SRGB && fmt->color_space == IPO_CS_SRGB &&
                     fmt->alpha_mode != IPO_ALPHA_PREMULTIPLIED);
    if (linear_srgb) {
        ipo_rgba64f_t pv = val;
        if (fmt->alpha_mode == IPO_ALPHA_PREMULTIPLIED && val.a < 1.0) {
            double ac = color_clamp01(val.a);
            pv.r *= ac; pv.g *= ac; pv.b *= ac; pv.a = ac;
        }
        switch (fmt->format_id) {
            case IPO_COLOR_FMT_RGBA64F: *(ipo_rgba64f_t *)pixel_ptr = pv; return;
            case IPO_COLOR_FMT_RGB64F: {
                double *d = (double *)pixel_ptr; d[0] = pv.r; d[1] = pv.g; d[2] = pv.b; return;
            }
            case IPO_COLOR_FMT_RGBA32F: {
                ipo_rgba32f_t *f = (ipo_rgba32f_t *)pixel_ptr;
                f->r = (float)pv.r; f->g = (float)pv.g; f->b = (float)pv.b; f->a = (float)pv.a; return;
            }
            case IPO_COLOR_FMT_RGB32F: {
                float *f = (float *)pixel_ptr; f[0] = (float)pv.r; f[1] = (float)pv.g; f[2] = (float)pv.b; return;
            }
            case IPO_COLOR_FMT_RGBA16F: {
                ipo_rgba16f_t *h = (ipo_rgba16f_t *)pixel_ptr;
                h->r = ipo_f64_to_f16(pv.r); h->g = ipo_f64_to_f16(pv.g);
                h->b = ipo_f64_to_f16(pv.b); h->a = ipo_f64_to_f16(pv.a); return;
            }
            default: break;
        }
    }
    if (srgb_std) {
        switch (fmt->format_id) {
            case IPO_COLOR_FMT_XRGB8888:
            case IPO_COLOR_FMT_ARGB8888: {
                uint8_t r8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.r) * 4095.0 + 0.5)];
                uint8_t g8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.g) * 4095.0 + 0.5)];
                uint8_t b8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.b) * 4095.0 + 0.5)];
                uint8_t a8 = (fmt->format_id == IPO_COLOR_FMT_ARGB8888) ? (uint8_t)(color_clamp01(val.a) * 255.0 + 0.5) : 0xFFu;
                *(uint32_t *)pixel_ptr = ((uint32_t)a8 << 24) | ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
                return;
            }
            case IPO_COLOR_FMT_RGBA8888: {
                uint8_t r8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.r) * 4095.0 + 0.5)];
                uint8_t g8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.g) * 4095.0 + 0.5)];
                uint8_t b8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.b) * 4095.0 + 0.5)];
                uint8_t a8 = (uint8_t)(color_clamp01(val.a) * 255.0 + 0.5);
                *(uint32_t *)pixel_ptr = ((uint32_t)a8 << 24) | ((uint32_t)b8 << 16) | ((uint32_t)g8 << 8) | r8;
                return;
            }
            case IPO_COLOR_FMT_RGB10A2: {
                uint32_t r10 = (uint32_t)s_lin12_to_srgb10_lut[(int)(color_clamp01(val.r) * 4095.0 + 0.5)];
                uint32_t g10 = (uint32_t)s_lin12_to_srgb10_lut[(int)(color_clamp01(val.g) * 4095.0 + 0.5)];
                uint32_t b10 = (uint32_t)s_lin12_to_srgb10_lut[(int)(color_clamp01(val.b) * 4095.0 + 0.5)];
                uint32_t a2 = (uint32_t)(color_clamp01(val.a) * 3.0 + 0.5);
                *(uint32_t *)pixel_ptr = (a2 << 30) | (b10 << 20) | (g10 << 10) | r10;
                return;
            }
            case IPO_COLOR_FMT_RGB888: {
                uint8_t *p = (uint8_t *)pixel_ptr;
                p[2] = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.r) * 4095.0 + 0.5)];
                p[1] = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.g) * 4095.0 + 0.5)];
                p[0] = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.b) * 4095.0 + 0.5)];
                return;
            }
            case IPO_COLOR_FMT_RGB565: {
                uint8_t r8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.r) * 4095.0 + 0.5)];
                uint8_t g8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.g) * 4095.0 + 0.5)];
                uint8_t b8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.b) * 4095.0 + 0.5)];
                *(uint16_t *)pixel_ptr = (uint16_t)(((uint32_t)(r8 >> 3) << 11) | ((uint32_t)(g8 >> 2) << 5) | (uint32_t)(b8 >> 3));
                return;
            }
            case IPO_COLOR_FMT_RGB555: {
                uint8_t r8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.r) * 4095.0 + 0.5)];
                uint8_t g8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.g) * 4095.0 + 0.5)];
                uint8_t b8 = s_lin12_to_srgb8_lut[(int)(color_clamp01(val.b) * 4095.0 + 0.5)];
                *(uint16_t *)pixel_ptr = (uint16_t)(((uint32_t)(r8 >> 3) << 10) | ((uint32_t)(g8 >> 3) << 5) | (uint32_t)(b8 >> 3));
                return;
            }
            default: break;
        }
    }
    ipo_rgba64f_t encoded = ipo_rgba64f_from_linear(val, fmt);
    uint8_t *p = (uint8_t *)pixel_ptr;
    memset(p, 0, fmt->bytes_per_pixel);
    for (uint32_t i = 0u; i < fmt->num_channels; i++) {
        const ipo_channel_desc_t *ch = &fmt->channels[i];
        double v = (ch->role == IPO_CH_ROLE_R) ? encoded.r :
                   (ch->role == IPO_CH_ROLE_G) ? encoded.g :
                   (ch->role == IPO_CH_ROLE_B) ? encoded.b :
                   (ch->role == IPO_CH_ROLE_A) ? encoded.a :
                   (0.2126 * encoded.r + 0.7152 * encoded.g + 0.0722 * encoded.b);
        if (ch->num_type == IPO_NUM_F64 || ch->num_type == IPO_NUM_F_EXT) write_bits_le(p, ch->bit_offset, 64u, f64_to_bits(v));
        else if (ch->num_type == IPO_NUM_F32) write_bits_le(p, ch->bit_offset, 32u, (uint64_t)f32_to_bits((float)v));
        else if (ch->num_type == IPO_NUM_F16) write_bits_le(p, ch->bit_offset, 16u, (uint64_t)ipo_f64_to_f16(v));
        else {
            uint32_t w = (ch->bit_width > 53u) ? 53u : ch->bit_width;
            uint64_t maxv = (w >= 64u) ? UINT64_MAX : ((1ULL << w) - 1ULL);
            uint64_t q = (uint64_t)(color_clamp01(v) * (double)maxv + 0.5);
            if (ch->bit_width > 53u) {
                uint32_t low_bits = ch->bit_width - 53u;
                uint64_t fill = (q >= maxv) ? UINT64_MAX : 0ULL;
                uint32_t off = 0u;
                while (off < low_bits) {
                    uint32_t chunk = (low_bits - off > 64u) ? 64u : (low_bits - off);
                    write_bits_le(p, ch->bit_offset + off, chunk, fill);
                    off += chunk;
                }
                write_bits_le(p, ch->bit_offset + low_bits, 53u, q);
            } else {
                write_bits_le(p, ch->bit_offset, w, q);
            }
        }
    }
}

void ipo_color_convert_scanline(const ipo_pixel_format_t *src_fmt, const void *src_row,
                                const ipo_pixel_format_t *dst_fmt, void *dst_row,
                                uint32_t pixel_count, ipo_tonemap_op_t tonemap,
                                const uint32_t *palette_xrgb8888) {
    if (!src_fmt || !dst_fmt || !src_row || !dst_row || pixel_count == 0u) return;
    ensure_color_luts();
    if (src_fmt->format_id != IPO_COLOR_FMT_CUSTOM &&
        src_fmt->format_id == dst_fmt->format_id && src_fmt->bpp == dst_fmt->bpp &&
        src_fmt->alpha_mode == dst_fmt->alpha_mode && src_fmt->transfer_fn == dst_fmt->transfer_fn &&
        src_fmt->color_space == dst_fmt->color_space && src_fmt->hdr_max_nits == dst_fmt->hdr_max_nits &&
        tonemap == IPO_TONEMAP_NONE && src_fmt->bytes_per_pixel > 0u) {
        memcpy(dst_row, src_row, (size_t)pixel_count * (size_t)src_fmt->bytes_per_pixel);
        return;
    }
    if (src_fmt->format_id == IPO_COLOR_FMT_RGBA64F &&
        src_fmt->transfer_fn == IPO_TF_LINEAR && src_fmt->color_space == IPO_CS_LINEAR_RGB &&
        (dst_fmt->format_id == IPO_COLOR_FMT_XRGB8888 || dst_fmt->format_id == IPO_COLOR_FMT_ARGB8888) &&
        dst_fmt->transfer_fn == IPO_TF_SRGB && dst_fmt->color_space == IPO_CS_SRGB) {
        const ipo_rgba64f_t *s = (const ipo_rgba64f_t *)src_row;
        uint32_t *d = (uint32_t *)dst_row;
        bool has_a = (dst_fmt->format_id == IPO_COLOR_FMT_ARGB8888);
        bool unpremul = (src_fmt->alpha_mode == IPO_ALPHA_PREMULTIPLIED &&
                         dst_fmt->alpha_mode == IPO_ALPHA_STRAIGHT);
        for (uint32_t i = 0u; i < pixel_count; i++) {
            ipo_rgba64f_t c = s[i];
            if (unpremul && c.a > 1e-12 && c.a < 1.0) {
                double inv_a = 1.0 / c.a;
                c.r *= inv_a; c.g *= inv_a; c.b *= inv_a;
            }
            if (tonemap != IPO_TONEMAP_NONE) c = ipo_rgba64f_tonemap(c, tonemap, 1.0);
            uint32_t a8 = has_a ? (uint32_t)(color_clamp01(c.a) * 255.0 + 0.5) : 0xFFu;
            d[i] = (a8 << 24) |
                   ((uint32_t)s_lin12_to_srgb8_lut[(int)(color_clamp01(c.r) * 4095.0 + 0.5)] << 16) |
                   ((uint32_t)s_lin12_to_srgb8_lut[(int)(color_clamp01(c.g) * 4095.0 + 0.5)] << 8) |
                   (uint32_t)s_lin12_to_srgb8_lut[(int)(color_clamp01(c.b) * 4095.0 + 0.5)];
        }
        return;
    }
    if (src_fmt->format_id == IPO_COLOR_FMT_INDEX8 &&
        (dst_fmt->format_id == IPO_COLOR_FMT_XRGB8888 || dst_fmt->format_id == IPO_COLOR_FMT_ARGB8888) &&
        palette_xrgb8888) {
        const uint8_t *s = (const uint8_t *)src_row;
        uint32_t *d = (uint32_t *)dst_row;
        for (uint32_t i = 0u; i < pixel_count; i++) d[i] = palette_xrgb8888[s[i]] | 0xFF000000u;
        return;
    }
    const uint8_t *sbytes = (const uint8_t *)src_row;
    uint8_t *dbytes = (uint8_t *)dst_row;
    size_t sstride = src_fmt->bytes_per_pixel ? src_fmt->bytes_per_pixel : ((src_fmt->bpp + 7u) >> 3);
    size_t dstride = dst_fmt->bytes_per_pixel ? dst_fmt->bytes_per_pixel : ((dst_fmt->bpp + 7u) >> 3);
    for (uint32_t i = 0u; i < pixel_count; i++) {
        ipo_rgba64f_t lin = ipo_color_unpack_rgba64f(src_fmt, sbytes + i * sstride, palette_xrgb8888);
        if (tonemap != IPO_TONEMAP_NONE) lin = ipo_rgba64f_tonemap(lin, tonemap, 1.0);
        ipo_color_pack_rgba64f(dst_fmt, dbytes + i * dstride, lin);
    }
}
