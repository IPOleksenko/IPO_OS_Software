#ifndef IPO_OH264_WRAP_H
#define IPO_OH264_WRAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ipo_h264_dec ipo_h264_dec_t;

ipo_h264_dec_t *ipo_h264_create(void);
void            ipo_h264_destroy(ipo_h264_dec_t *dec);
void            ipo_h264_flush(ipo_h264_dec_t *dec);
int             ipo_h264_set_avcc(ipo_h264_dec_t *dec, const uint8_t *avcc, size_t avcc_len);

/* Decodes one MP4 length-prefixed (or Annex-B if no avcC was set) H.264 access unit.
 * If a picture is ready, converts YUV420p -> XRGB32 aspect-fit inside (dst_w, dst_h)
 * and returns 1 (writing *out_ts, *out_src_w, *out_src_h). Returns 0 if buffering, -1 on error. */
int             ipo_h264_decode_xrgb32(ipo_h264_dec_t *dec,
                                       const uint8_t *sample, size_t sample_len,
                                       uint64_t in_ts, uint64_t *out_ts,
                                       uint32_t *dst_xrgb32, int dst_w, int dst_h,
                                       int *out_src_w, int *out_src_h);

/* Flushes any remaining picture buffered in the decoder at EOF (returns 1 if produced, 0 if none). */
int             ipo_h264_drain_xrgb32(ipo_h264_dec_t *dec,
                                      uint64_t *out_ts,
                                      uint32_t *dst_xrgb32, int dst_w, int dst_h,
                                      int *out_src_w, int *out_src_h);

#ifdef __cplusplus
}
#endif

#endif
