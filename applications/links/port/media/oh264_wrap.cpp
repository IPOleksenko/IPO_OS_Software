#include "oh264_wrap.h"

#include <stdlib.h>
#include <string.h>

#include "codec_api.h"
#include "WelsThreadLib.h"

/* -------------------------------------------------------------------------
 * Single-threaded OpenH264 OS/Thread primitives for IPO_OS freestanding env
 * ------------------------------------------------------------------------- */
extern "C" {

WELS_THREAD_ERROR_CODE WelsMutexInit (WELS_MUTEX* mutex) {
    if (mutex) *mutex = 0;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_ERROR_CODE WelsMutexLock (WELS_MUTEX* mutex) {
    (void)mutex;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_ERROR_CODE WelsMutexUnlock (WELS_MUTEX* mutex) {
    (void)mutex;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_ERROR_CODE WelsMutexDestroy (WELS_MUTEX* mutex) {
    (void)mutex;
    return WELS_THREAD_ERROR_OK;
}

WELS_THREAD_ERROR_CODE WelsEventOpen (WELS_EVENT* p_event, const char* event_name) {
    (void)event_name;
    if (p_event) *p_event = NULL;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_ERROR_CODE WelsEventClose (WELS_EVENT* event, const char* event_name) {
    (void)event;
    (void)event_name;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_ERROR_CODE WelsEventSignal (WELS_EVENT* event, WELS_MUTEX* pMutex, int* iCondition) {
    (void)event;
    (void)pMutex;
    if (iCondition) *iCondition = 1;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_ERROR_CODE WelsEventWait (WELS_EVENT* event, WELS_MUTEX* pMutex, int& iCondition) {
    (void)event;
    (void)pMutex;
    (void)iCondition;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_ERROR_CODE WelsEventWaitWithTimeOut (WELS_EVENT* event, uint32_t dwMilliseconds, WELS_MUTEX* pMutex) {
    (void)event;
    (void)dwMilliseconds;
    (void)pMutex;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_ERROR_CODE WelsMultipleEventsWaitSingleBlocking (uint32_t nCount, WELS_EVENT* event_list,
                                                             WELS_EVENT* master_event, WELS_MUTEX* pMutex) {
    (void)nCount;
    (void)event_list;
    (void)master_event;
    (void)pMutex;
    return WELS_THREAD_ERROR_WAIT_OBJECT_0;
}
WELS_THREAD_ERROR_CODE WelsThreadCreate (WELS_THREAD_HANDLE* thread, LPWELS_THREAD_ROUTINE routine,
                                         void* arg, WELS_THREAD_ATTR attr) {
    (void)routine;
    (void)arg;
    (void)attr;
    if (thread) *thread = 0;
    return WELS_THREAD_ERROR_GENERAL;
}
WELS_THREAD_ERROR_CODE WelsThreadSetName (const char* thread_name) {
    (void)thread_name;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_ERROR_CODE WelsThreadJoin (WELS_THREAD_HANDLE thread) {
    (void)thread;
    return WELS_THREAD_ERROR_OK;
}
WELS_THREAD_HANDLE WelsThreadSelf (void) {
    return (WELS_THREAD_HANDLE)1;
}
WELS_THREAD_ERROR_CODE WelsQueryLogicalProcessInfo (WelsLogicalProcessInfo* pInfo) {
    if (pInfo) pInfo->ProcessorCount = 1;
    return WELS_THREAD_ERROR_OK;
}
void WelsSleep (uint32_t dwMilliSecond) {
    (void)dwMilliSecond;
}

} /* extern "C" */

/* -------------------------------------------------------------------------
 * OpenH264 Decoder Wrapper + Fast Integer Aspect-Fit YUV420p -> XRGB32 Scaler
 * ------------------------------------------------------------------------- */
struct ipo_h264_dec {
    ISVCDecoder *codec;
    uint8_t     *param_buf;
    size_t       param_len;
    uint8_t     *au_buf;
    size_t       au_cap;
    int          nal_len_size;
    int          need_params;
    int          has_avcc;
    int         *x_map;
    int          x_map_w;
    int          x_map_src_w;
};

static ISVCDecoder *oh264_open_codec(void) {
    ISVCDecoder *codec = NULL;
    if (WelsCreateDecoder(&codec) != 0 || !codec) return NULL;
    int log_level = WELS_LOG_QUIET;
    codec->SetOption(DECODER_OPTION_TRACE_LEVEL, &log_level);
    int threads = 0;
    codec->SetOption(DECODER_OPTION_NUM_OF_THREADS, &threads);
    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.sVideoProperty.size = sizeof(param.sVideoProperty);
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    if (codec->Initialize(&param) != 0) {
        WelsDestroyDecoder(codec);
        return NULL;
    }
    return codec;
}

static int ensure_au_cap(ipo_h264_dec_t *d, size_t need) {
    if (need <= d->au_cap) return 0;
    size_t nc = d->au_cap ? d->au_cap : 65536u;
    while (nc < need) nc *= 2u;
    uint8_t *nb = (uint8_t *)realloc(d->au_buf, nc);
    if (!nb) return -1;
    d->au_buf = nb;
    d->au_cap = nc;
    return 0;
}

static inline uint8_t clamp_u8(int v) {
    if (v <= 0) return 0;
    if (v >= 255) return 255;
    return (uint8_t)v;
}

static void convert_i420_to_xrgb32(ipo_h264_dec_t *d,
                                   const SBufferInfo *info,
                                   unsigned char *const planes[3],
                                   uint32_t *dst_xrgb32, int dst_w, int dst_h,
                                   int *out_src_w, int *out_src_h) {
    const SSysMEMBuffer *pic = &info->UsrData.sSystemBuffer;
    int sw = pic->iWidth;
    int sh = pic->iHeight;
    if (out_src_w) *out_src_w = sw;
    if (out_src_h) *out_src_h = sh;
    if (!dst_xrgb32 || dst_w <= 0 || dst_h <= 0 || sw <= 0 || sh <= 0 || !planes[0]) return;

    int stride_y = pic->iStride[0];
    int stride_uv = pic->iStride[1];
    const uint8_t *py = planes[0];
    const uint8_t *pu = planes[1];
    const uint8_t *pv = planes[2];
    int has_uv = (pu != NULL && pv != NULL && stride_uv > 0);

    /* Fit cleanly into dst_w x dst_h preserving aspect ratio */
    int dw = dst_w;
    int dh = (int)(((long long)dst_w * (long long)sh) / sw);
    if (dh > dst_h) {
        dh = dst_h;
        dw = (int)(((long long)dst_h * (long long)sw) / sh);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = (dst_w - dw) / 2;
    int oy = (dst_h - dh) / 2;

    if (ox > 0 || oy > 0) {
        size_t total = (size_t)dst_w * (size_t)dst_h;
        for (size_t i = 0; i < total; i++) dst_xrgb32[i] = 0xFF000000u;
    }

    if (dw > d->x_map_w) {
        int *nm = (int *)realloc(d->x_map, (size_t)dw * sizeof(int));
        if (nm) {
            d->x_map = nm;
            d->x_map_w = dw;
            d->x_map_src_w = -1;
        }
    }
    if (d->x_map && d->x_map_src_w != (sw << 16) + dw) {
        for (int x = 0; x < dw; x++) d->x_map[x] = (x * sw) / dw;
        d->x_map_src_w = (sw << 16) + dw;
    }

    /* Fixed-point (shift 8): BT.601 for <720p, BT.709 for >=720p (limited YUV range) */
    const int y_mul = 298;
    const int rv_c  = (sh >= 720) ? 459 : 409;
    const int gu_c  = (sh >= 720) ? 55  : 100;
    const int gv_c  = (sh >= 720) ? 136 : 208;
    const int bu_c  = (sh >= 720) ? 541 : 516;

    int prev_sy = -1;
    uint32_t *prev_out = NULL;
    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        uint32_t *out = dst_xrgb32 + (size_t)(oy + y) * (size_t)dst_w + ox;
        if (sy == prev_sy && prev_out) {
            memcpy(out, prev_out, (size_t)dw * sizeof(uint32_t));
            continue;
        }
        const uint8_t *y_row = py + (size_t)sy * (size_t)stride_y;
        const uint8_t *u_row = has_uv ? (pu + (size_t)(sy >> 1) * (size_t)stride_uv) : NULL;
        const uint8_t *v_row = has_uv ? (pv + (size_t)(sy >> 1) * (size_t)stride_uv) : NULL;

        if (u_row && v_row) {
            int last_cx = -1;
            int r_off = 0, g_off = 0, b_off = 0;
            for (int x = 0; x < dw; x++) {
                int sx = d->x_map ? d->x_map[x] : ((x * sw) / dw);
                int cx = sx >> 1;
                if (cx != last_cx) {
                    last_cx = cx;
                    int u = (int)u_row[cx] - 128;
                    int v = (int)v_row[cx] - 128;
                    r_off = rv_c * v;
                    g_off = -(gu_c * u + gv_c * v);
                    b_off = bu_c * u;
                }
                int yy = (int)y_row[sx] - 16;
                if (yy < 0) yy = 0;
                int y298 = yy * y_mul + 128;
                uint8_t r = clamp_u8((y298 + r_off) >> 8);
                uint8_t g = clamp_u8((y298 + g_off) >> 8);
                uint8_t b = clamp_u8((y298 + b_off) >> 8);
                out[x] = 0x80000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        } else {
            for (int x = 0; x < dw; x++) {
                int sx = d->x_map ? d->x_map[x] : ((x * sw) / dw);
                int yy = (int)y_row[sx] - 16;
                if (yy < 0) yy = 0;
                uint8_t l = clamp_u8((yy * y_mul + 128) >> 8);
                out[x] = 0x80000000u | ((uint32_t)l << 16) | ((uint32_t)l << 8) | (uint32_t)l;
            }
        }
        prev_sy = sy;
        prev_out = out;
    }
}

extern "C" ipo_h264_dec_t *ipo_h264_create(void) {
    ISVCDecoder *codec = oh264_open_codec();
    if (!codec) return NULL;
    ipo_h264_dec_t *d = (ipo_h264_dec_t *)calloc(1, sizeof(ipo_h264_dec_t));
    if (!d) {
        codec->Uninitialize();
        WelsDestroyDecoder(codec);
        return NULL;
    }
    d->codec = codec;
    d->nal_len_size = 4;
    return d;
}

extern "C" void ipo_h264_destroy(ipo_h264_dec_t *d) {
    if (!d) return;
    if (d->codec) {
        d->codec->Uninitialize();
        WelsDestroyDecoder(d->codec);
    }
    free(d->param_buf);
    free(d->au_buf);
    free(d->x_map);
    free(d);
}

extern "C" void ipo_h264_flush(ipo_h264_dec_t *d) {
    if (!d) return;
    if (d->codec) {
        d->codec->Uninitialize();
        WelsDestroyDecoder(d->codec);
    }
    d->codec = oh264_open_codec();
    d->need_params = d->has_avcc;
}

extern "C" int ipo_h264_set_avcc(ipo_h264_dec_t *d, const uint8_t *avcc, size_t avcc_len) {
    if (!d) return -1;
    free(d->param_buf);
    d->param_buf = NULL;
    d->param_len = 0;
    d->has_avcc = 0;
    d->need_params = 0;
    d->nal_len_size = 4;

    if (!avcc || avcc_len == 0) return 0;
    if (avcc_len < 7 || avcc[0] != 1) return -1;

    int lsz = (avcc[4] & 3) + 1;
    size_t pos = 5;
    size_t out_cap = avcc_len + 64u;
    uint8_t *pbuf = (uint8_t *)malloc(out_cap);
    if (!pbuf) return -1;
    size_t out_len = 0;

    for (int g = 0; g < 2; g++) {
        if (pos >= avcc_len) { free(pbuf); return -1; }
        unsigned cnt = (g == 0) ? (avcc[pos] & 0x1Fu) : avcc[pos];
        pos++;
        for (unsigned i = 0; i < cnt; i++) {
            if (avcc_len - pos < 2u) { free(pbuf); return -1; }
            size_t nsz = ((size_t)avcc[pos] << 8) | (size_t)avcc[pos + 1];
            pos += 2u;
            if (nsz > avcc_len - pos) { free(pbuf); return -1; }
            if (out_len + 4u + nsz > out_cap) {
                out_cap = out_len + 4u + nsz + 64u;
                uint8_t *nb = (uint8_t *)realloc(pbuf, out_cap);
                if (!nb) { free(pbuf); return -1; }
                pbuf = nb;
            }
            pbuf[out_len++] = 0;
            pbuf[out_len++] = 0;
            pbuf[out_len++] = 0;
            pbuf[out_len++] = 1;
            if (nsz) {
                memcpy(pbuf + out_len, avcc + pos, nsz);
                out_len += nsz;
            }
            pos += nsz;
        }
    }

    d->param_buf = pbuf;
    d->param_len = out_len;
    d->nal_len_size = lsz;
    d->has_avcc = 1;
    d->need_params = 1;
    return 0;
}

extern "C" int ipo_h264_decode_xrgb32(ipo_h264_dec_t *d,
                                      const uint8_t *sample, size_t sample_len,
                                      uint64_t in_ts, uint64_t *out_ts,
                                      uint32_t *dst_xrgb32, int dst_w, int dst_h,
                                      int *out_src_w, int *out_src_h) {
    if (!d || !d->codec || !sample || sample_len == 0) return -1;

    size_t max_out = (d->need_params ? d->param_len : 0) + sample_len + 4096u;
    if (ensure_au_cap(d, max_out) != 0) return -1;

    size_t out_len = 0;
    if (d->need_params && d->param_buf && d->param_len > 0) {
        memcpy(d->au_buf, d->param_buf, d->param_len);
        out_len += d->param_len;
        d->need_params = 0;
    }

    if (d->has_avcc) {
        size_t pos = 0;
        int lsz = d->nal_len_size;
        while (pos < sample_len) {
            if (sample_len - pos < (size_t)lsz) return -1;
            size_t nsz = 0;
            for (int i = 0; i < lsz; i++) nsz = (nsz << 8) | sample[pos++];
            if (nsz > sample_len - pos) return -1;
            if (nsz == 0) continue;
            if (ensure_au_cap(d, out_len + 4u + nsz) != 0) return -1;
            d->au_buf[out_len++] = 0;
            d->au_buf[out_len++] = 0;
            d->au_buf[out_len++] = 0;
            d->au_buf[out_len++] = 1;
            memcpy(d->au_buf + out_len, sample + pos, nsz);
            out_len += nsz;
            pos += nsz;
        }
    } else {
        memcpy(d->au_buf + out_len, sample, sample_len);
        out_len += sample_len;
    }

    if (out_len == 0) return 0;

    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    info.uiInBsTimeStamp = (unsigned long long)in_ts;
    unsigned char *planes[3] = { NULL, NULL, NULL };

    DECODING_STATE st = d->codec->DecodeFrameNoDelay(d->au_buf, (int)out_len, planes, &info);
    if (info.iBufferStatus == 1) {
        if (out_ts) *out_ts = (uint64_t)info.uiOutYuvTimeStamp;
        convert_i420_to_xrgb32(d, &info, planes, dst_xrgb32, dst_w, dst_h, out_src_w, out_src_h);
        return 1;
    }
    return (st == dsErrorFree || st == dsDataErrorConcealed) ? 0 : -1;
}

extern "C" int ipo_h264_drain_xrgb32(ipo_h264_dec_t *d,
                                     uint64_t *out_ts,
                                     uint32_t *dst_xrgb32, int dst_w, int dst_h,
                                     int *out_src_w, int *out_src_h) {
    if (!d || !d->codec) return 0;
    int remaining = 0;
    d->codec->GetOption(DECODER_OPTION_NUM_OF_FRAMES_REMAINING_IN_BUFFER, &remaining);
    if (remaining <= 0) return 0;
    int eos = 1;
    d->codec->SetOption(DECODER_OPTION_END_OF_STREAM, &eos);
    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    unsigned char *planes[3] = { NULL, NULL, NULL };
    d->codec->FlushFrame(planes, &info);
    if (info.iBufferStatus == 1) {
        if (out_ts) *out_ts = (uint64_t)info.uiOutYuvTimeStamp;
        convert_i420_to_xrgb32(d, &info, planes, dst_xrgb32, dst_w, dst_h, out_src_w, out_src_h);
        return 1;
    }
    return 0;
}
