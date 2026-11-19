#include "cfg.h"

#ifdef G
#include "links.h"

#ifdef HAVE_WUFFS

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* Provided by third_party/northstar_media/image_wuffs.c compiled with the local GLib shim. */
extern uint8_t *ns_image_wuffs_decode_to_bgra(const uint8_t *data, size_t len,
                                             int *out_w, int *out_h,
                                             size_t *out_stride,
                                             size_t *out_buf_len);

/* Unused by Links; satisfies reference inside image_wuffs.c's ns_image_decode_wuffs. */
void *ns_texture_new(int width, int height, int format, void *bytes, size_t stride)
{
    (void)width;
    (void)height;
    (void)format;
    (void)bytes;
    (void)stride;
    return NULL;
}

struct wuffs_img_decoder {
    unsigned char *buffer;
    size_t len;
    size_t cap;
};

void wuffs_img_start(struct cached_image *cimg)
{
    struct wuffs_img_decoder *deco = mem_calloc(sizeof(struct wuffs_img_decoder));
    cimg->decoder = deco;
}

void wuffs_img_restart(struct cached_image *cimg, unsigned char *data, int length)
{
    struct wuffs_img_decoder *deco = (struct wuffs_img_decoder *)cimg->decoder;
    if (!deco || !data || length <= 0) return;

    size_t add = (size_t)length;
    if (deco->len + add > deco->cap) {
        size_t ncap = deco->cap ? deco->cap : 8192u;
        while (ncap < deco->len + add) {
            if (ncap > (size_t)MAXINT / 2u) {
                ncap = deco->len + add;
                break;
            }
            ncap *= 2u;
        }
        unsigned char *nbuf = deco->buffer
            ? (unsigned char *)mem_realloc(deco->buffer, ncap)
            : (unsigned char *)mem_alloc(ncap);
        if (!nbuf) return;
        deco->buffer = nbuf;
        deco->cap = ncap;
    }
    memcpy(deco->buffer + deco->len, data, add);
    deco->len += add;
}

void wuffs_img_destroy_decoder(struct cached_image *cimg)
{
    struct wuffs_img_decoder *deco = (struct wuffs_img_decoder *)cimg->decoder;
    if (deco && deco->buffer) {
        mem_free(deco->buffer);
        deco->buffer = NULL;
        deco->len = 0;
        deco->cap = 0;
    }
}

void wuffs_img_finish(struct cached_image *cimg)
{
    struct wuffs_img_decoder *deco = (struct wuffs_img_decoder *)cimg->decoder;
    int w = 0, h = 0;
    size_t stride = 0, buf_len = 0;
    uint8_t *bgra;

    if (!deco || !deco->buffer || deco->len == 0) {
        return;
    }

    bgra = ns_image_wuffs_decode_to_bgra((const uint8_t *)deco->buffer,
                                         deco->len,
                                         &w, &h, &stride, &buf_len);
    mem_free(deco->buffer);
    deco->buffer = NULL;
    deco->len = 0;
    deco->cap = 0;

    if (!bgra || w <= 0 || h <= 0 || stride < (size_t)w * 4u) {
        if (bgra) free(bgra);
        return;
    }

    cimg->width = w;
    cimg->height = h;
    cimg->buffer_bytes_per_pixel = 4;
    cimg->red_gamma = cimg->green_gamma = cimg->blue_gamma = (float)sRGB_gamma;
    cimg->strip_optimized = 0;
    if (header_dimensions_known(cimg)) {
        free(bgra);
        return;
    }

    /* Convert premultiplied BGRA -> straight RGBA expected by Links dip.c */
    for (int y = 0; y < h; y++) {
        const uint8_t *src = bgra + (size_t)y * stride;
        unsigned char *dst = cimg->buffer + (size_t)y * (size_t)w * 4u;
        for (int x = 0; x < w; x++) {
            uint8_t b = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t r = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            if (a > 0 && a < 255) {
                unsigned ur = ((unsigned)r * 255u + (a >> 1)) / a;
                unsigned ug = ((unsigned)g * 255u + (a >> 1)) / a;
                unsigned ub = ((unsigned)b * 255u + (a >> 1)) / a;
                r = (uint8_t)(ur > 255u ? 255u : ur);
                g = (uint8_t)(ug > 255u ? 255u : ug);
                b = (uint8_t)(ub > 255u ? 255u : ub);
            }
            dst[x * 4 + 0] = r;
            dst[x * 4 + 1] = g;
            dst[x * 4 + 2] = b;
            dst[x * 4 + 3] = a;
        }
    }

    free(bgra);
}

#endif /* HAVE_WUFFS */
#endif /* G */
