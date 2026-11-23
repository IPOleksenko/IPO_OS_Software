#ifndef LIB_DRIVER_AUDIO_FP_H
#define LIB_DRIVER_AUDIO_FP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/**
 * @brief Rounding modes for arbitrary fixed-point arithmetic and conversions.
 */
typedef enum {
    AUDIO_FP_ROUND_TRUNC     = 0, /* Round toward zero */
    AUDIO_FP_ROUND_FLOOR     = 1, /* Round toward -infinity */
    AUDIO_FP_ROUND_HALF_UP   = 2, /* Nearest, ties away from zero */
    AUDIO_FP_ROUND_HALF_EVEN = 3  /* Nearest, ties to even */
} audio_fp_round_t;

/**
 * @brief Overflow handling modes for arbitrary fixed-point arithmetic.
 */
typedef enum {
    AUDIO_FP_OVERFLOW_SATURATE = 0, /* Clamp to representable range [min, max] */
    AUDIO_FP_OVERFLOW_WRAP     = 1  /* Modular wrap to total_bits width */
} audio_fp_overflow_t;

#define AUDIO_FP_FLAG_OVERFLOW    (1u << 0)
#define AUDIO_FP_FLAG_UNDERFLOW   (1u << 1)
#define AUDIO_FP_FLAG_INEXACT     (1u << 2)
#define AUDIO_FP_FLAG_DIV_ZERO    (1u << 3)
#define AUDIO_FP_FLAG_INVALID_FMT (1u << 4)

/**
 * @brief Configurable fixed-point number format descriptor.
 *
 * Supports signed/unsigned representations, zero or non-zero fractional bits
 * (0..64), and arbitrary standard or non-standard total bit widths (1..64).
 */
typedef struct {
    uint8_t int_bits;      /* Integer magnitude bits (0..64) */
    uint8_t frac_bits;     /* Fractional bits (0..64) */
    uint8_t total_bits;    /* Total width in bits (1..64; 0 = auto from int+frac+sign) */
    uint8_t is_signed;     /* 0 = unsigned, 1 = two's complement signed */
    uint8_t round_mode;    /* audio_fp_round_t */
    uint8_t overflow_mode; /* audio_fp_overflow_t */
    uint16_t reserved;
} audio_fp_format_t;

/**
 * @brief Normalized 64-bit container holding an arbitrary fixed-point value.
 *
 * For unsigned formats, upper bits above total_bits are zero.
 * For signed formats, bit (total_bits - 1) is sign-extended through bit 63.
 */
typedef struct {
    uint64_t raw;
} audio_fp_t;

/**
 * @brief Runtime fixed-point configuration for the audio subsystem.
 */
typedef struct {
    audio_fp_format_t pos_format; /* Stream playback cursor, phase, and step ratio */
    audio_fp_format_t mix_format; /* Sample normalization, gain, and mixing */
} audio_fp_config_t;

bool audio_fp_format_normalize(const audio_fp_format_t *in, audio_fp_format_t *out);
bool audio_fp_format_is_valid(const audio_fp_format_t *fmt);

audio_fp_format_t audio_fp_format_make(uint8_t int_bits, uint8_t frac_bits, bool is_signed,
                                       audio_fp_round_t round_mode,
                                       audio_fp_overflow_t overflow_mode);
audio_fp_format_t audio_fp_format_make_ex(uint8_t total_bits, uint8_t int_bits, uint8_t frac_bits,
                                          bool is_signed, audio_fp_round_t round_mode,
                                          audio_fp_overflow_t overflow_mode);
void audio_fp_default_config(audio_fp_config_t *cfg);

audio_fp_t audio_fp_zero(void);
audio_fp_t audio_fp_one(const audio_fp_format_t *fmt, uint32_t *flags);
audio_fp_t audio_fp_min_val(const audio_fp_format_t *fmt);
audio_fp_t audio_fp_max_val(const audio_fp_format_t *fmt);

audio_fp_t audio_fp_from_raw(const audio_fp_format_t *fmt, uint64_t raw, uint32_t *flags);
audio_fp_t audio_fp_from_i64(const audio_fp_format_t *fmt, int64_t val, uint32_t *flags);
audio_fp_t audio_fp_from_u64(const audio_fp_format_t *fmt, uint64_t val, uint32_t *flags);
audio_fp_t audio_fp_from_ratio(const audio_fp_format_t *fmt, int64_t num, int64_t den, uint32_t *flags);
audio_fp_t audio_fp_from_u_ratio(const audio_fp_format_t *fmt, uint64_t num, uint64_t den, uint32_t *flags);
audio_fp_t audio_fp_from_f32_bits(const audio_fp_format_t *fmt, uint32_t ieee_bits, uint32_t *flags);
audio_fp_t audio_fp_from_f64_bits(const audio_fp_format_t *fmt, uint64_t ieee_bits, uint32_t *flags);

int64_t  audio_fp_to_i64(const audio_fp_format_t *fmt, audio_fp_t val,
                         audio_fp_round_t round_mode, uint32_t *flags);
uint64_t audio_fp_to_u64(const audio_fp_format_t *fmt, audio_fp_t val,
                         audio_fp_round_t round_mode, uint32_t *flags);
uint32_t audio_fp_to_f32_bits(const audio_fp_format_t *fmt, audio_fp_t val);
uint64_t audio_fp_to_f64_bits(const audio_fp_format_t *fmt, audio_fp_t val);

audio_fp_t audio_fp_neg(const audio_fp_format_t *fmt, audio_fp_t a, uint32_t *flags);
audio_fp_t audio_fp_abs(const audio_fp_format_t *fmt, audio_fp_t a, uint32_t *flags);
audio_fp_t audio_fp_add(const audio_fp_format_t *fmt, audio_fp_t a, audio_fp_t b, uint32_t *flags);
audio_fp_t audio_fp_sub(const audio_fp_format_t *fmt, audio_fp_t a, audio_fp_t b, uint32_t *flags);
audio_fp_t audio_fp_mul(const audio_fp_format_t *fmt, audio_fp_t a, audio_fp_t b, uint32_t *flags);
audio_fp_t audio_fp_div(const audio_fp_format_t *fmt, audio_fp_t a, audio_fp_t b, uint32_t *flags);
audio_fp_t audio_fp_scale_ratio(const audio_fp_format_t *fmt, audio_fp_t a,
                                int64_t num, int64_t den, uint32_t *flags);

audio_fp_t audio_fp_clamp(const audio_fp_format_t *fmt, audio_fp_t val, audio_fp_t lo, audio_fp_t hi);
int        audio_fp_cmp(const audio_fp_format_t *fmt, audio_fp_t a, audio_fp_t b);
audio_fp_t audio_fp_frac_part(const audio_fp_format_t *fmt, audio_fp_t val);

audio_fp_t audio_fp_convert(audio_fp_t val, const audio_fp_format_t *src_fmt,
                            const audio_fp_format_t *dst_fmt, uint32_t *flags);
audio_fp_t audio_fp_mul_mixed(audio_fp_t a, const audio_fp_format_t *a_fmt,
                              audio_fp_t b, const audio_fp_format_t *b_fmt,
                              const audio_fp_format_t *dst_fmt, uint32_t *flags);

audio_fp_t audio_fp_pcm_midpoint(const audio_fp_format_t *fmt);
audio_fp_t audio_fp_pcm_s16_to_fp(const audio_fp_format_t *fmt, int16_t sample, uint32_t *flags);
audio_fp_t audio_fp_pcm_u8_to_fp(const audio_fp_format_t *fmt, uint8_t sample, uint32_t *flags);
audio_fp_t audio_fp_pcm_s24_to_fp(const audio_fp_format_t *fmt, int32_t sample24, uint32_t *flags);
audio_fp_t audio_fp_pcm_s32_to_fp(const audio_fp_format_t *fmt, int32_t sample32, uint32_t *flags);
audio_fp_t audio_fp_pcm_scale_vol(const audio_fp_format_t *fmt, audio_fp_t sample,
                                  uint32_t vol_num, uint32_t vol_den, uint32_t *flags);
audio_fp_t audio_fp_pcm_mix_add(const audio_fp_format_t *fmt, audio_fp_t acc,
                                audio_fp_t sample, uint32_t *flags);
int16_t    audio_fp_to_pcm_s16(const audio_fp_format_t *fmt, audio_fp_t val, uint32_t *flags);

#endif /* LIB_DRIVER_AUDIO_FP_H */
