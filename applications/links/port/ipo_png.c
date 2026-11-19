#include <png.h>
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

struct png_info_def {
    png_uint_32 width;
    png_uint_32 height;
    png_byte bit_depth;
    png_byte color_type;
    png_byte interlace_type;
    uint32_t valid;
    double gamma;
    int srgb_intent;
    uint8_t plte[256][3];
    int plte_count;
    uint8_t trns_alpha[256];
    int trns_count;
    uint16_t trns_gray;
    uint16_t trns_r, trns_g, trns_b;
};

struct png_struct_def {
    jmp_buf jmpbuf;
    png_voidp error_ptr;
    png_error_ptr error_fn;
    png_error_ptr warn_fn;
    png_voidp mem_ptr;
    png_malloc_ptr malloc_fn;
    png_free_ptr free_fn;

    /* Pull IO */
    png_voidp io_ptr;
    png_rw_ptr read_fn;
    int hdr_has_chunk;
    uint32_t pull_chunk_len;
    uint8_t pull_chunk_type[4];
    int saw_iend;

    /* Progressive IO */
    png_voidp progressive_ptr;
    png_progressive_info_ptr prog_info_fn;
    png_progressive_row_ptr prog_row_fn;
    png_progressive_end_ptr prog_end_fn;
    int prog_stage; /* 0=sig, 1=chunk_hdr, 2=chunk_data, 3=chunk_crc, 4=done */
    uint8_t hdr_tmp[8];
    size_t hdr_tmp_pos;
    uint32_t prog_chunk_len;
    uint32_t prog_chunk_pos;
    uint8_t prog_chunk_type[4];
    uint8_t *chunk_buf;
    size_t chunk_cap;
    size_t crc_pos;

    /* Transform flags */
    int want_expand;
    int want_strip_16;
    int want_strip_alpha;
    int want_rgb_to_gray;
    int want_gray_to_rgb;
    int want_swap;
    int want_interlace;
    int out_channels;
    int out_bit_depth;

    /* Zlib inflate state */
    png_infop active_info;
    z_stream z;
    int z_init;
    int z_done;

    /* Row decoding state */
    int cur_pass;
    int cur_row;
    uint8_t *raw_row;
    uint8_t *prev_row;
    size_t raw_row_bytes;
    size_t raw_row_pos;
    uint8_t *out_row;
    size_t out_row_cap;
    int filter_bpp;
};

static void *png_xmalloc(png_structp png, size_t sz) {
    if (sz == 0) sz = 1;
    if (png && png->malloc_fn) return png->malloc_fn(png, sz);
    return malloc(sz);
}

static void png_xfree(png_structp png, void *p) {
    if (!p) return;
    if (png && png->free_fn) png->free_fn(png, p);
    else free(p);
}

jmp_buf *ipo_png_jmpbuf_ptr(png_structp png_ptr) {
    return &png_ptr->jmpbuf;
}

void png_longjmp(png_structp png_ptr, int val) {
    longjmp(png_ptr->jmpbuf, val ? val : 1);
}

void png_error(png_structp png_ptr, png_const_charp error_message) {
    if (png_ptr && png_ptr->error_fn) {
        png_ptr->error_fn(png_ptr, error_message ? error_message : "PNG error");
    }
    if (png_ptr) longjmp(png_ptr->jmpbuf, 1);
}

png_structp png_create_read_struct_2(png_const_charp user_png_ver, png_voidp error_ptr,
                                     png_error_ptr error_fn, png_error_ptr warn_fn,
                                     png_voidp mem_ptr, png_malloc_ptr malloc_fn,
                                     png_free_ptr free_fn) {
    (void)user_png_ver;
    png_structp s = (png_structp)(malloc_fn ? malloc_fn(NULL, sizeof(*s)) : malloc(sizeof(*s)));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->error_ptr = error_ptr;
    s->error_fn = error_fn;
    s->warn_fn = warn_fn;
    s->mem_ptr = mem_ptr;
    s->malloc_fn = malloc_fn;
    s->free_fn = free_fn;
    return s;
}

png_structp png_create_read_struct(png_const_charp user_png_ver, png_voidp error_ptr,
                                   png_error_ptr error_fn, png_error_ptr warn_fn) {
    return png_create_read_struct_2(user_png_ver, error_ptr, error_fn, warn_fn, NULL, NULL, NULL);
}

png_infop png_create_info_struct(png_structp png_ptr) {
    png_infop info = (png_infop)png_xmalloc(png_ptr, sizeof(*info));
    if (!info) return NULL;
    memset(info, 0, sizeof(*info));
    info->gamma = 0.45455;
    return info;
}

void png_destroy_read_struct(png_structpp png_ptr_ptr, png_infopp info_ptr_ptr,
                             png_infopp end_info_ptr_ptr) {
    png_structp p = (png_ptr_ptr && *png_ptr_ptr) ? *png_ptr_ptr : NULL;
    if (info_ptr_ptr && *info_ptr_ptr) {
        png_xfree(p, *info_ptr_ptr);
        *info_ptr_ptr = NULL;
    }
    if (end_info_ptr_ptr && *end_info_ptr_ptr) {
        png_xfree(p, *end_info_ptr_ptr);
        *end_info_ptr_ptr = NULL;
    }
    if (p) {
        if (p->z_init) inflateEnd(&p->z);
        png_xfree(p, p->chunk_buf);
        png_xfree(p, p->raw_row);
        png_xfree(p, p->prev_row);
        png_xfree(p, p->out_row);
        png_free_ptr ffn = p->free_fn;
        if (ffn) ffn(p, p);
        else free(p);
        *png_ptr_ptr = NULL;
    }
}

void png_set_read_fn(png_structp png_ptr, png_voidp io_ptr, png_rw_ptr read_data_fn) {
    if (!png_ptr) return;
    png_ptr->io_ptr = io_ptr;
    png_ptr->read_fn = read_data_fn;
}

png_voidp png_get_io_ptr(png_structp png_ptr) {
    return png_ptr ? png_ptr->io_ptr : NULL;
}

void png_set_progressive_read_fn(png_structp png_ptr, png_voidp progressive_ptr,
                                 png_progressive_info_ptr info_fn,
                                 png_progressive_row_ptr row_fn,
                                 png_progressive_end_ptr end_fn) {
    if (!png_ptr) return;
    png_ptr->progressive_ptr = progressive_ptr;
    png_ptr->prog_info_fn = info_fn;
    png_ptr->prog_row_fn = row_fn;
    png_ptr->prog_end_fn = end_fn;
}

png_voidp png_get_progressive_ptr(png_structp png_ptr) {
    return png_ptr ? png_ptr->progressive_ptr : NULL;
}

static const int adam7_xstart[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const int adam7_ystart[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const int adam7_xinc[7]   = { 8, 8, 4, 4, 2, 2, 1 };
static const int adam7_yinc[7]   = { 8, 8, 8, 4, 4, 2, 2 };

static int pass_dims(png_infop info, int pass, int *pw, int *ph) {
    if (info->interlace_type == PNG_INTERLACE_NONE) {
        *pw = (int)info->width;
        *ph = (int)info->height;
        return (*pw > 0 && *ph > 0);
    }
    int w = (int)info->width;
    int h = (int)info->height;
    int xs = adam7_xstart[pass], ys = adam7_ystart[pass];
    int xi = adam7_xinc[pass], yi = adam7_yinc[pass];
    *pw = (w > xs) ? (w - xs + xi - 1) / xi : 0;
    *ph = (h > ys) ? (h - ys + yi - 1) / yi : 0;
    return (*pw > 0 && *ph > 0);
}

static int src_channels_for(png_byte color_type) {
    switch (color_type) {
        case PNG_COLOR_TYPE_GRAY: return 1;
        case PNG_COLOR_TYPE_PALETTE: return 1;
        case PNG_COLOR_TYPE_RGB: return 3;
        case PNG_COLOR_TYPE_GRAY_ALPHA: return 2;
        case PNG_COLOR_TYPE_RGB_ALPHA: return 4;
        default: return 1;
    }
}

static void compute_output_layout(png_structp png, png_infop info) {
    int bd = info->bit_depth;
    int ct = info->color_type;

    if (png->want_expand) {
        if (ct == PNG_COLOR_TYPE_PALETTE) {
            ct = (info->valid & PNG_INFO_tRNS) ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB;
            bd = 8;
        } else if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) {
            bd = 8;
        }
        if ((info->valid & PNG_INFO_tRNS) && (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_RGB)) {
            ct |= PNG_COLOR_MASK_ALPHA;
        }
    }
    if (bd == 16 && png->want_strip_16) {
        bd = 8;
    }
    if (png->want_strip_alpha) {
        ct &= ~PNG_COLOR_MASK_ALPHA;
    }
    if (png->want_gray_to_rgb && !(ct & PNG_COLOR_MASK_COLOR) && ct != PNG_COLOR_TYPE_PALETTE) {
        ct |= PNG_COLOR_MASK_COLOR;
    }
    if (png->want_rgb_to_gray && (ct & PNG_COLOR_MASK_COLOR)) {
        ct &= ~PNG_COLOR_MASK_COLOR;
    }

    png->out_channels = src_channels_for((png_byte)ct);
    png->out_bit_depth = bd;
}

static size_t setup_pass_buffers(png_structp png, png_infop info) {
    int pw = 0, ph = 0;
    int npasses = (info->interlace_type == PNG_INTERLACE_ADAM7) ? 7 : 1;
    while (png->cur_pass < npasses) {
        if (pass_dims(info, png->cur_pass, &pw, &ph)) break;
        png->cur_pass++;
        png->cur_row = 0;
    }
    if (png->cur_pass >= npasses) return 0;

    int src_ch = src_channels_for(info->color_type);
    int bpp_bits = src_ch * info->bit_depth;
    int filter_bpp = (bpp_bits + 7) / 8;
    if (filter_bpp < 1) filter_bpp = 1;
    size_t row_bytes = ((size_t)pw * (size_t)bpp_bits + 7u) / 8u;

    if (row_bytes != png->raw_row_bytes) {
        png_xfree(png, png->raw_row);
        png_xfree(png, png->prev_row);
        png->raw_row = (uint8_t *)png_xmalloc(png, 1 + row_bytes);
        png->prev_row = (uint8_t *)png_xmalloc(png, 1 + row_bytes);
        if (!png->raw_row || !png->prev_row) png_error(png, "OOM row");
        png->raw_row_bytes = row_bytes;
    }
    if (png->cur_row == 0) {
        memset(png->prev_row, 0, 1 + row_bytes);
    }
    png->filter_bpp = filter_bpp;
    return row_bytes;
}

static uint8_t paeth_pred(int a, int b, int c) {
    int p = a + b - c;
    int pa = p - a; if (pa < 0) pa = -pa;
    int pb = p - b; if (pb < 0) pb = -pb;
    int pc = p - c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

static void unfilter_row(uint8_t filter, uint8_t *cur, const uint8_t *prev, size_t len, int bpp) {
    switch (filter) {
        case 0: break;
        case 1:
            for (size_t i = (size_t)bpp; i < len; i++) {
                cur[i] = (uint8_t)(cur[i] + cur[i - bpp]);
            }
            break;
        case 2:
            for (size_t i = 0; i < len; i++) {
                cur[i] = (uint8_t)(cur[i] + prev[i]);
            }
            break;
        case 3:
            for (size_t i = 0; i < len; i++) {
                int a = (i >= (size_t)bpp) ? cur[i - bpp] : 0;
                int b = prev[i];
                cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (size_t i = 0; i < len; i++) {
                int a = (i >= (size_t)bpp) ? cur[i - bpp] : 0;
                int b = prev[i];
                int c = (i >= (size_t)bpp) ? prev[i - bpp] : 0;
                cur[i] = (uint8_t)(cur[i] + paeth_pred(a, b, c));
            }
            break;
        default:
            break;
    }
}

static void transform_row(png_structp png, png_infop info, int pw, const uint8_t *src, uint8_t *dst) {
    compute_output_layout(png, info);
    int out_ch = png->out_channels;
    int out_bd = png->out_bit_depth;
    int src_bd = info->bit_depth;
    int src_ct = info->color_type;
    int src_ch = src_channels_for((png_byte)src_ct);

    for (int x = 0; x < pw; x++) {
        uint16_t r = 0, g = 0, b = 0, a = 65535;
        int is_16 = (src_bd == 16);

        if (src_ct == PNG_COLOR_TYPE_PALETTE) {
            uint8_t idx = 0;
            if (src_bd == 8) {
                idx = src[x];
            } else {
                int ppb = 8 / src_bd;
                int shift = (ppb - 1 - (x % ppb)) * src_bd;
                idx = (uint8_t)((src[x / ppb] >> shift) & ((1 << src_bd) - 1));
            }
            r = (uint16_t)info->plte[idx][0] * 257u;
            g = (uint16_t)info->plte[idx][1] * 257u;
            b = (uint16_t)info->plte[idx][2] * 257u;
            if ((info->valid & PNG_INFO_tRNS) && idx < info->trns_count) {
                a = (uint16_t)info->trns_alpha[idx] * 257u;
            }
        } else if (src_bd < 8) {
            int ppb = 8 / src_bd;
            int shift = (ppb - 1 - (x % ppb)) * src_bd;
            uint32_t v = (src[x / ppb] >> shift) & (uint32_t)((1 << src_bd) - 1);
            uint32_t maxv = (uint32_t)((1 << src_bd) - 1);
            uint16_t gv = (uint16_t)((v * 255u + (maxv / 2u)) / maxv) * 257u;
            r = g = b = gv;
            if ((info->valid & PNG_INFO_tRNS) && v == info->trns_gray) a = 0;
        } else if (src_bd == 8) {
            const uint8_t *px = src + x * src_ch;
            if (src_ct == PNG_COLOR_TYPE_GRAY) {
                r = g = b = (uint16_t)px[0] * 257u;
                if ((info->valid & PNG_INFO_tRNS) && px[0] == info->trns_gray) a = 0;
            } else if (src_ct == PNG_COLOR_TYPE_GRAY_ALPHA) {
                r = g = b = (uint16_t)px[0] * 257u;
                a = (uint16_t)px[1] * 257u;
            } else if (src_ct == PNG_COLOR_TYPE_RGB) {
                r = (uint16_t)px[0] * 257u;
                g = (uint16_t)px[1] * 257u;
                b = (uint16_t)px[2] * 257u;
                if ((info->valid & PNG_INFO_tRNS) &&
                    px[0] == info->trns_r && px[1] == info->trns_g && px[2] == info->trns_b) {
                    a = 0;
                }
            } else {
                r = (uint16_t)px[0] * 257u;
                g = (uint16_t)px[1] * 257u;
                b = (uint16_t)px[2] * 257u;
                a = (uint16_t)px[3] * 257u;
            }
        } else {
            const uint8_t *px = src + x * src_ch * 2;
            if (src_ct == PNG_COLOR_TYPE_GRAY) {
                uint16_t v = (uint16_t)(((uint16_t)px[0] << 8) | px[1]);
                r = g = b = v;
                if ((info->valid & PNG_INFO_tRNS) && v == info->trns_gray) a = 0;
            } else if (src_ct == PNG_COLOR_TYPE_GRAY_ALPHA) {
                r = g = b = (uint16_t)(((uint16_t)px[0] << 8) | px[1]);
                a = (uint16_t)(((uint16_t)px[2] << 8) | px[3]);
            } else if (src_ct == PNG_COLOR_TYPE_RGB) {
                r = (uint16_t)(((uint16_t)px[0] << 8) | px[1]);
                g = (uint16_t)(((uint16_t)px[2] << 8) | px[3]);
                b = (uint16_t)(((uint16_t)px[4] << 8) | px[5]);
                if ((info->valid & PNG_INFO_tRNS) &&
                    r == info->trns_r && g == info->trns_g && b == info->trns_b) {
                    a = 0;
                }
            } else {
                r = (uint16_t)(((uint16_t)px[0] << 8) | px[1]);
                g = (uint16_t)(((uint16_t)px[2] << 8) | px[3]);
                b = (uint16_t)(((uint16_t)px[4] << 8) | px[5]);
                a = (uint16_t)(((uint16_t)px[6] << 8) | px[7]);
            }
        }

        uint16_t gray16 = (uint16_t)((77u * r + 150u * g + 29u * b + 128u) >> 8);
        if (!is_16 || out_bd == 8) {
            uint8_t *d = dst + x * out_ch;
            uint8_t r8 = (uint8_t)(r >> 8), g8 = (uint8_t)(g >> 8), b8 = (uint8_t)(b >> 8), a8 = (uint8_t)(a >> 8);
            uint8_t y8 = (uint8_t)(gray16 >> 8);
            if (out_ch == 1) d[0] = y8;
            else if (out_ch == 2) { d[0] = y8; d[1] = a8; }
            else if (out_ch == 3) { d[0] = r8; d[1] = g8; d[2] = b8; }
            else { d[0] = r8; d[1] = g8; d[2] = b8; d[3] = a8; }
        } else {
            uint16_t vals[4];
            if (out_ch <= 2) { vals[0] = gray16; vals[1] = a; }
            else { vals[0] = r; vals[1] = g; vals[2] = b; vals[3] = a; }
            uint8_t *d = dst + x * out_ch * 2;
            for (int c = 0; c < out_ch; c++) {
                if (png->want_swap) {
                    d[c * 2 + 0] = (uint8_t)(vals[c] & 0xFFu);
                    d[c * 2 + 1] = (uint8_t)(vals[c] >> 8);
                } else {
                    d[c * 2 + 0] = (uint8_t)(vals[c] >> 8);
                    d[c * 2 + 1] = (uint8_t)(vals[c] & 0xFFu);
                }
            }
        }
    }
}

static void handle_chunk(png_structp png, png_infop info, const uint8_t type[4],
                         const uint8_t *data, uint32_t len) {
    if (memcmp(type, "IHDR", 4) == 0 && len >= 13) {
        info->width = read_be32(data);
        info->height = read_be32(data + 4);
        info->bit_depth = data[8];
        info->color_type = data[9];
        info->interlace_type = data[12];
        if (info->width == 0 || info->height == 0 || info->width > 16384 || info->height > 16384) {
            png_error(png, "Invalid PNG dimensions");
        }
        if (png->prog_info_fn) {
            png->active_info = info;
            png->prog_info_fn(png, info);
        }
    } else if (memcmp(type, "PLTE", 4) == 0) {
        int n = (int)(len / 3u);
        if (n > 256) n = 256;
        info->plte_count = n;
        for (int i = 0; i < n; i++) {
            info->plte[i][0] = data[i * 3 + 0];
            info->plte[i][1] = data[i * 3 + 1];
            info->plte[i][2] = data[i * 3 + 2];
        }
    } else if (memcmp(type, "tRNS", 4) == 0) {
        info->valid |= PNG_INFO_tRNS;
        if (info->color_type == PNG_COLOR_TYPE_PALETTE) {
            int n = (int)len;
            if (n > 256) n = 256;
            for (int i = 0; i < 256; i++) info->trns_alpha[i] = 255;
            for (int i = 0; i < n; i++) info->trns_alpha[i] = data[i];
            info->trns_count = n;
        } else if (info->color_type == PNG_COLOR_TYPE_GRAY && len >= 2) {
            info->trns_gray = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
        } else if (info->color_type == PNG_COLOR_TYPE_RGB && len >= 6) {
            info->trns_r = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
            info->trns_g = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
            info->trns_b = (uint16_t)(((uint16_t)data[4] << 8) | data[5]);
        }
    } else if (memcmp(type, "gAMA", 4) == 0 && len >= 4) {
        uint32_t g = read_be32(data);
        if (g > 0) {
            info->gamma = (double)g / 100000.0;
            info->valid |= PNG_INFO_gAMA;
        }
    } else if (memcmp(type, "sRGB", 4) == 0 && len >= 1) {
        info->srgb_intent = data[0];
        info->valid |= PNG_INFO_sRGB;
    }
}

static void feed_idat_progressive(png_structp png, png_infop info, const uint8_t *data, size_t len) {
    if (!png->z_init) {
        memset(&png->z, 0, sizeof(png->z));
        if (inflateInit(&png->z) != Z_OK) png_error(png, "inflateInit failed");
        png->z_init = 1;
    }
    if (png->z_done || len == 0) return;

    png->z.next_in = (Bytef *)data;
    png->z.avail_in = (uInt)len;

    while (png->z.avail_in > 0 && !png->z_done) {
        size_t row_bytes = setup_pass_buffers(png, info);
        if (row_bytes == 0) break;

        size_t need = (1 + row_bytes) - png->raw_row_pos;
        png->z.next_out = png->raw_row + png->raw_row_pos;
        png->z.avail_out = (uInt)need;
        int zr = inflate(&png->z, Z_NO_FLUSH);
        size_t got = need - (size_t)png->z.avail_out;
        png->raw_row_pos += got;

        if (png->raw_row_pos == 1 + row_bytes) {
            int pw = 0, ph = 0;
            pass_dims(info, png->cur_pass, &pw, &ph);
            unfilter_row(png->raw_row[0], png->raw_row + 1, png->prev_row + 1, row_bytes, png->filter_bpp);
            memcpy(png->prev_row + 1, png->raw_row + 1, row_bytes);

            compute_output_layout(png, info);
            size_t px_sz = (size_t)png->out_channels * (png->out_bit_depth == 16 ? 2u : 1u);
            size_t out_bytes = (size_t)pw * px_sz;
            if (out_bytes > png->out_row_cap) {
                png_xfree(png, png->out_row);
                png->out_row = (uint8_t *)png_xmalloc(png, out_bytes ? out_bytes : 1);
                png->out_row_cap = out_bytes;
            }
            transform_row(png, info, pw, png->raw_row + 1, png->out_row);

            uint32_t img_row = (info->interlace_type == PNG_INTERLACE_ADAM7)
                ? (uint32_t)(adam7_ystart[png->cur_pass] + png->cur_row * adam7_yinc[png->cur_pass])
                : (uint32_t)png->cur_row;

            if (png->prog_row_fn) {
                png->active_info = info;
                png->prog_row_fn(png, png->out_row, img_row, png->cur_pass);
            }

            png->raw_row_pos = 0;
            png->cur_row++;
            if (png->cur_row >= ph) {
                png->cur_pass++;
                png->cur_row = 0;
            }
        }
        if (zr == Z_STREAM_END) {
            png->z_done = 1;
            break;
        }
        if (zr != Z_OK && zr != Z_BUF_ERROR) {
            png_error(png, "PNG zlib error");
        }
    }
}

void png_process_data(png_structp png_ptr, png_infop info_ptr,
                      png_bytep buffer, png_size_t buffer_size) {
    if (!png_ptr || !info_ptr || !buffer || buffer_size == 0) return;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    size_t pos = 0;
    png_ptr->active_info = info_ptr;

    while (pos < buffer_size && png_ptr->prog_stage != 4) {
        if (png_ptr->prog_stage == 0) {
            size_t need = 8 - png_ptr->hdr_tmp_pos;
            size_t take = (buffer_size - pos < need) ? (buffer_size - pos) : need;
            memcpy(png_ptr->hdr_tmp + png_ptr->hdr_tmp_pos, buffer + pos, take);
            png_ptr->hdr_tmp_pos += take;
            pos += take;
            if (png_ptr->hdr_tmp_pos == 8) {
                if (memcmp(png_ptr->hdr_tmp, sig, 8) != 0) png_error(png_ptr, "Not a PNG");
                png_ptr->hdr_tmp_pos = 0;
                png_ptr->prog_stage = 1;
            }
        } else if (png_ptr->prog_stage == 1) {
            size_t need = 8 - png_ptr->hdr_tmp_pos;
            size_t take = (buffer_size - pos < need) ? (buffer_size - pos) : need;
            memcpy(png_ptr->hdr_tmp + png_ptr->hdr_tmp_pos, buffer + pos, take);
            png_ptr->hdr_tmp_pos += take;
            pos += take;
            if (png_ptr->hdr_tmp_pos == 8) {
                png_ptr->prog_chunk_len = read_be32(png_ptr->hdr_tmp);
                memcpy(png_ptr->prog_chunk_type, png_ptr->hdr_tmp + 4, 4);
                png_ptr->hdr_tmp_pos = 0;
                png_ptr->prog_chunk_pos = 0;
                if (memcmp(png_ptr->prog_chunk_type, "IDAT", 4) != 0 &&
                    png_ptr->prog_chunk_len > 0 &&
                    png_ptr->prog_chunk_len > png_ptr->chunk_cap &&
                    png_ptr->prog_chunk_len <= (1u << 20)) {
                    png_xfree(png_ptr, png_ptr->chunk_buf);
                    png_ptr->chunk_buf = (uint8_t *)png_xmalloc(png_ptr, png_ptr->prog_chunk_len);
                    png_ptr->chunk_cap = png_ptr->prog_chunk_len;
                }
                png_ptr->prog_stage = (png_ptr->prog_chunk_len > 0) ? 2 : 3;
                png_ptr->crc_pos = 0;
                if (png_ptr->prog_chunk_len == 0 && memcmp(png_ptr->prog_chunk_type, "IEND", 4) == 0) {
                    /* Finalized when the trailer CRC completes */
                }
            }
        } else if (png_ptr->prog_stage == 2) {
            size_t rem = (size_t)(png_ptr->prog_chunk_len - png_ptr->prog_chunk_pos);
            size_t take = (buffer_size - pos < rem) ? (buffer_size - pos) : rem;
            if (memcmp(png_ptr->prog_chunk_type, "IDAT", 4) == 0) {
                feed_idat_progressive(png_ptr, info_ptr, buffer + pos, take);
            } else if (png_ptr->chunk_buf && png_ptr->prog_chunk_pos + take <= png_ptr->chunk_cap) {
                memcpy(png_ptr->chunk_buf + png_ptr->prog_chunk_pos, buffer + pos, take);
            }
            png_ptr->prog_chunk_pos += (uint32_t)take;
            pos += take;
            if (png_ptr->prog_chunk_pos == png_ptr->prog_chunk_len) {
                if (memcmp(png_ptr->prog_chunk_type, "IDAT", 4) != 0) {
                    handle_chunk(png_ptr, info_ptr, png_ptr->prog_chunk_type,
                                 png_ptr->chunk_buf, png_ptr->prog_chunk_len);
                }
                png_ptr->prog_stage = 3;
                png_ptr->crc_pos = 0;
            }
        } else if (png_ptr->prog_stage == 3) {
            size_t need = 4 - png_ptr->crc_pos;
            size_t take = (buffer_size - pos < need) ? (buffer_size - pos) : need;
            png_ptr->crc_pos += take;
            pos += take;
            if (png_ptr->crc_pos == 4) {
                png_ptr->crc_pos = 0;
                if (memcmp(png_ptr->prog_chunk_type, "IEND", 4) == 0) {
                    png_ptr->prog_stage = 4;
                    if (png_ptr->prog_end_fn) png_ptr->prog_end_fn(png_ptr, info_ptr);
                } else {
                    png_ptr->prog_stage = 1;
                }
            }
        }
    }
}

void png_progressive_combine_row(png_structp png_ptr, png_bytep old_row,
                                 png_const_bytep new_row) {
    if (!png_ptr || !old_row || !new_row) return;
    png_infop info = png_ptr->active_info;
    compute_output_layout(png_ptr, info);
    size_t px_sz = (size_t)png_ptr->out_channels * (png_ptr->out_bit_depth == 16 ? 2u : 1u);
    if (!info || info->interlace_type == PNG_INTERLACE_NONE) {
        size_t n = info ? (size_t)info->width * px_sz : 0;
        if (n) memcpy(old_row, new_row, n);
        return;
    }
    int pass = png_ptr->cur_pass;
    if (pass < 0 || pass >= 7) return;
    int xs = adam7_xstart[pass], xi = adam7_xinc[pass];
    int sx = 0;
    for (int x = xs; x < (int)info->width; x += xi, sx++) {
        memcpy(old_row + (size_t)x * px_sz, new_row + (size_t)sx * px_sz, px_sz);
    }
}

/* Pull-mode helpers (used by dip.c for embedded fonts) */
static void pull_bytes(png_structp png, uint8_t *dst, size_t len) {
    if (!png->read_fn) png_error(png, "No read_fn");
    png->read_fn(png, dst, len);
}

void png_read_info(png_structp png_ptr, png_infop info_ptr) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t got[8];
    if (!png_ptr || !info_ptr) return;
    png_ptr->active_info = info_ptr;
    pull_bytes(png_ptr, got, 8);
    if (memcmp(got, sig, 8) != 0) png_error(png_ptr, "Bad PNG sig");

    for (;;) {
        uint8_t ch[8];
        pull_bytes(png_ptr, ch, 8);
        uint32_t clen = read_be32(ch);
        if (memcmp(ch + 4, "IDAT", 4) == 0) {
            png_ptr->hdr_has_chunk = 1;
            png_ptr->pull_chunk_len = clen;
            memcpy(png_ptr->pull_chunk_type, ch + 4, 4);
            break;
        }
        if (memcmp(ch + 4, "IEND", 4) == 0) {
            uint8_t crc[4];
            if (clen > 0) {
                uint8_t *tmp = (uint8_t *)png_xmalloc(png_ptr, clen);
                pull_bytes(png_ptr, tmp, clen);
                png_xfree(png_ptr, tmp);
            }
            pull_bytes(png_ptr, crc, 4);
            png_ptr->saw_iend = 1;
            break;
        }
        uint8_t *buf = clen ? (uint8_t *)png_xmalloc(png_ptr, clen) : NULL;
        if (clen) pull_bytes(png_ptr, buf, clen);
        uint8_t crc[4];
        pull_bytes(png_ptr, crc, 4);
        if (clen || memcmp(ch + 4, "PLTE", 4) != 0) {
            handle_chunk(png_ptr, info_ptr, ch + 4, buf, clen);
        }
        if (buf) png_xfree(png_ptr, buf);
    }
}

void png_read_update_info(png_structp png_ptr, png_infop info_ptr) {
    if (!png_ptr || !info_ptr) return;
    png_ptr->active_info = info_ptr;
    compute_output_layout(png_ptr, info_ptr);
}

static uint8_t pull_next_idat_chunk(png_structp png) {
    uint8_t crc[4];
    pull_bytes(png, crc, 4); /* finish previous chunk CRC */
    for (;;) {
        uint8_t ch[8];
        pull_bytes(png, ch, 8);
        uint32_t clen = read_be32(ch);
        if (memcmp(ch + 4, "IDAT", 4) == 0) {
            png->pull_chunk_len = clen;
            if (clen > 0) return 1;
            pull_bytes(png, crc, 4);
            continue;
        }
        if (memcmp(ch + 4, "IEND", 4) == 0) {
            pull_bytes(png, crc, 4);
            png->saw_iend = 1;
            png->pull_chunk_len = 0;
            return 0;
        }
        if (clen) {
            uint8_t *tmp = (uint8_t *)png_xmalloc(png, clen);
            pull_bytes(png, tmp, clen);
            png_xfree(png, tmp);
        }
        pull_bytes(png, crc, 4);
    }
}

void png_read_rows(png_structp png_ptr, png_bytepp row, png_bytepp display_row,
                   png_uint_32 num_rows) {
    (void)display_row;
    if (!png_ptr || !png_ptr->active_info || num_rows == 0) return;
    png_infop info = png_ptr->active_info;

    if (!png_ptr->z_init) {
        memset(&png_ptr->z, 0, sizeof(png_ptr->z));
        if (inflateInit(&png_ptr->z) != Z_OK) png_error(png_ptr, "inflateInit fail");
        png_ptr->z_init = 1;
    }

    uint32_t pass_rows_left = 0;
    if (info->interlace_type == PNG_INTERLACE_ADAM7) {
        int pw = 0, ph = 0;
        setup_pass_buffers(png_ptr, info);
        pass_dims(info, png_ptr->cur_pass, &pw, &ph);
        pass_rows_left = (uint32_t)(ph - png_ptr->cur_row);
    } else {
        pass_rows_left = num_rows;
    }
    if (pass_rows_left == 0) return;

    uint8_t inbuf[2048];
    uint32_t done_rows = 0;
    uint32_t target_rows = (info->interlace_type == PNG_INTERLACE_ADAM7) ? pass_rows_left : num_rows;

    while (done_rows < target_rows) {
        size_t row_bytes = setup_pass_buffers(png_ptr, info);
        if (row_bytes == 0) break;

        if (png_ptr->z.avail_in == 0 && !png_ptr->z_done) {
            if (png_ptr->pull_chunk_len == 0 && !png_ptr->saw_iend) {
                if (!pull_next_idat_chunk(png_ptr)) png_ptr->z_done = 1;
            }
            if (png_ptr->pull_chunk_len > 0) {
                size_t want = png_ptr->pull_chunk_len < sizeof(inbuf) ? png_ptr->pull_chunk_len : sizeof(inbuf);
                pull_bytes(png_ptr, inbuf, want);
                png_ptr->pull_chunk_len -= (uint32_t)want;
                png_ptr->z.next_in = inbuf;
                png_ptr->z.avail_in = (uInt)want;
            }
        }

        size_t need = (1 + row_bytes) - png_ptr->raw_row_pos;
        png_ptr->z.next_out = png_ptr->raw_row + png_ptr->raw_row_pos;
        png_ptr->z.avail_out = (uInt)need;
        int zr = inflate(&png_ptr->z, Z_NO_FLUSH);
        size_t got = need - (size_t)png_ptr->z.avail_out;
        png_ptr->raw_row_pos += got;

        if (png_ptr->raw_row_pos == 1 + row_bytes) {
            int pw = 0, ph = 0;
            pass_dims(info, png_ptr->cur_pass, &pw, &ph);
            unfilter_row(png_ptr->raw_row[0], png_ptr->raw_row + 1, png_ptr->prev_row + 1, row_bytes, png_ptr->filter_bpp);
            memcpy(png_ptr->prev_row + 1, png_ptr->raw_row + 1, row_bytes);

            compute_output_layout(png_ptr, info);
            size_t px_sz = (size_t)png_ptr->out_channels * (png_ptr->out_bit_depth == 16 ? 2u : 1u);
            if (info->interlace_type == PNG_INTERLACE_NONE) {
                uint32_t img_r = (uint32_t)png_ptr->cur_row;
                if (row && img_r < info->height && row[img_r]) {
                    transform_row(png_ptr, info, pw, png_ptr->raw_row + 1, row[img_r]);
                }
            } else {
                size_t ob = (size_t)pw * px_sz;
                if (ob > png_ptr->out_row_cap) {
                    png_xfree(png_ptr, png_ptr->out_row);
                    png_ptr->out_row = (uint8_t *)png_xmalloc(png_ptr, ob ? ob : 1);
                    png_ptr->out_row_cap = ob;
                }
                transform_row(png_ptr, info, pw, png_ptr->raw_row + 1, png_ptr->out_row);
                uint32_t img_r = (uint32_t)(adam7_ystart[png_ptr->cur_pass] + png_ptr->cur_row * adam7_yinc[png_ptr->cur_pass]);
                if (row && img_r < info->height && row[img_r]) {
                    png_progressive_combine_row(png_ptr, row[img_r], png_ptr->out_row);
                }
            }
            png_ptr->raw_row_pos = 0;
            png_ptr->cur_row++;
            done_rows++;
            if (png_ptr->cur_row >= ph) {
                png_ptr->cur_pass++;
                png_ptr->cur_row = 0;
            }
        }
        if (zr == Z_STREAM_END) {
            png_ptr->z_done = 1;
            break;
        }
        if (zr != Z_OK && zr != Z_BUF_ERROR) {
            png_error(png_ptr, "PNG inflate error");
        }
    }
}

void png_read_end(png_structp png_ptr, png_infop info_ptr) {
    (void)png_ptr;
    (void)info_ptr;
}

png_uint_32 png_get_image_width(png_structp png_ptr, png_infop info_ptr) {
    (void)png_ptr; return info_ptr ? info_ptr->width : 0;
}
png_uint_32 png_get_image_height(png_structp png_ptr, png_infop info_ptr) {
    (void)png_ptr; return info_ptr ? info_ptr->height : 0;
}
png_byte png_get_color_type(png_structp png_ptr, png_infop info_ptr) {
    (void)png_ptr; return info_ptr ? info_ptr->color_type : 0;
}
png_byte png_get_bit_depth(png_structp png_ptr, png_infop info_ptr) {
    (void)png_ptr; return info_ptr ? info_ptr->bit_depth : 8;
}
png_byte png_get_interlace_type(png_structp png_ptr, png_infop info_ptr) {
    (void)png_ptr; return info_ptr ? info_ptr->interlace_type : PNG_INTERLACE_NONE;
}
png_uint_32 png_get_valid(png_structp png_ptr, png_infop info_ptr, png_uint_32 flag) {
    (void)png_ptr; return info_ptr ? (info_ptr->valid & flag) : 0;
}
png_uint_32 png_get_gAMA(png_structp png_ptr, png_infop info_ptr, double *file_gamma) {
    (void)png_ptr;
    if (!info_ptr || !(info_ptr->valid & PNG_INFO_gAMA)) return 0;
    if (file_gamma) *file_gamma = info_ptr->gamma;
    return 1;
}
png_uint_32 png_get_sRGB(png_structp png_ptr, png_infop info_ptr, int *file_srgb_intent) {
    (void)png_ptr;
    if (!info_ptr || !(info_ptr->valid & PNG_INFO_sRGB)) return 0;
    if (file_srgb_intent) *file_srgb_intent = info_ptr->srgb_intent;
    return 1;
}

void png_set_gamma(png_structp png_ptr, double screen_gamma, double override_file_gamma) {
    (void)png_ptr; (void)screen_gamma; (void)override_file_gamma;
}
void png_set_expand(png_structp png_ptr) {
    if (png_ptr) png_ptr->want_expand = 1;
}
void png_set_strip_16(png_structp png_ptr) {
    if (png_ptr) png_ptr->want_strip_16 = 1;
}
void png_set_strip_alpha(png_structp png_ptr) {
    if (png_ptr) png_ptr->want_strip_alpha = 1;
}
void png_set_rgb_to_gray(png_structp png_ptr, int error_action,
                         double red_weight, double green_weight) {
    (void)error_action; (void)red_weight; (void)green_weight;
    if (png_ptr) png_ptr->want_rgb_to_gray = 1;
}
void png_set_gray_to_rgb(png_structp png_ptr) {
    if (png_ptr) png_ptr->want_gray_to_rgb = 1;
}
void png_set_swap(png_structp png_ptr) {
    if (png_ptr) png_ptr->want_swap = 1;
}
int png_set_interlace_handling(png_structp png_ptr) {
    if (!png_ptr) return 1;
    png_ptr->want_interlace = 1;
    if (png_ptr->active_info && png_ptr->active_info->interlace_type == PNG_INTERLACE_ADAM7) {
        return 7;
    }
    return 1;
}
png_const_charp png_get_libpng_ver(png_structp png_ptr) {
    (void)png_ptr;
    return PNG_LIBPNG_VER_STRING;
}
