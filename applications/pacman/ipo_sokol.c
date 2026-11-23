#define SOKOL_LETTERBOX_IMPL
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_audio.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_letterbox.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <wm.h>
#include <vga.h>
#include <vga_gfx.h>
#include <driver/input/keyboard.h>
#include <system/state.h>
#include <system/timer.h>
#include <driver/audio_core.h>
#include <ipo/audio.h>
#include <driver/sound.h>
#include <syscall.h>
#include <kernel/input_ext.h>
#include "common/ipo_hd_present.h"

extern int serial_printf(const char *fmt, ...);
extern sapp_desc sokol_main(int argc, char *argv[]);

/* =========================================================================
 * Virtual Playfield + Palette Mapping (O(1) RGB555 -> IPO_OS 248-color cube)
 * ========================================================================= */
#define PAC_W 224
#define PAC_H 288
#define CANVAS_43_W 384
#define CANVAS_43_H 288

typedef struct {
    float x, y;
    float u, v;
    uint32_t attr;
} pac_vertex_t;

static uint8_t s_wm_palette[248][3];
static uint32_t s_wm_pal32[256];
static uint8_t s_rgb555_to_wm[32768];
static bool    s_pal_ready = false;

static void init_wm_palette_lut(void) {
    if (s_pal_ready) return;
    static const uint8_t cga[16][3] = {
        {0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
        {170, 0, 0},     {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
        {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
        {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255}
    };
    for (int i = 0; i < 16; i++) {
        s_wm_palette[i][0] = cga[i][0];
        s_wm_palette[i][1] = cga[i][1];
        s_wm_palette[i][2] = cga[i][2];
    }
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)((i * 255) / 15);
        s_wm_palette[16 + i][0] = v;
        s_wm_palette[16 + i][1] = v;
        s_wm_palette[16 + i][2] = v;
    }
    int idx = 32;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                s_wm_palette[idx][0] = (uint8_t)(r * 51);
                s_wm_palette[idx][1] = (uint8_t)(g * 51);
                s_wm_palette[idx][2] = (uint8_t)(b * 51);
                idx++;
            }
        }
    }
    for (int r5 = 0; r5 < 32; r5++) {
        int r = (r5 << 3) | (r5 >> 2);
        for (int g5 = 0; g5 < 32; g5++) {
            int g = (g5 << 3) | (g5 >> 2);
            for (int b5 = 0; b5 < 32; b5++) {
                int b = (b5 << 3) | (b5 >> 2);
                int best = 0;
                uint32_t best_d = 0xFFFFFFFFu;
                for (int i = 0; i < 248; i++) {
                    int dr = r - (int)s_wm_palette[i][0];
                    int dg = g - (int)s_wm_palette[i][1];
                    int db = b - (int)s_wm_palette[i][2];
                    uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
                    if (d < best_d) {
                        best_d = d;
                        best = i;
                        if (d == 0) break;
                    }
                }
                s_rgb555_to_wm[(r5 << 10) | (g5 << 5) | b5] = (uint8_t)best;
            }
        }
    }
    for (int i = 0; i < 248; i++) {
        s_wm_pal32[i] = 0xFF000000u |
                        ((uint32_t)s_wm_palette[i][0] << 16) |
                        ((uint32_t)s_wm_palette[i][1] << 8)  |
                        (uint32_t)s_wm_palette[i][2];
    }
    for (int i = 248; i < 256; i++) s_wm_pal32[i] = 0xFF000000u;
    s_pal_ready = true;
}

static inline uint8_t rgb_to_wm(uint8_t r, uint8_t g, uint8_t b) {
    return s_rgb555_to_wm[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
}

static void apply_wm_dac_palette(void) {
    init_wm_palette_lut();
    for (int i = 0; i < 248; i++) {
        vga_gfx_set_palette((uint8_t)i,
                            (uint8_t)(s_wm_palette[i][0] >> 2),
                            (uint8_t)(s_wm_palette[i][1] >> 2),
                            (uint8_t)(s_wm_palette[i][2] >> 2));
    }
}

/* =========================================================================
 * Sokol GFX Software Backend
 * ========================================================================= */
typedef struct {
    bool used;
    int w, h;
    sg_pixel_format fmt;
    bool is_target;
    uint8_t *data;
    uint8_t pal_wm[256];
} sg_img_obj_t;

typedef struct {
    bool used;
    uint32_t img_id;
} sg_view_obj_t;

typedef struct {
    bool used;
    const void *data;
    size_t size;
} sg_buf_obj_t;

static sg_img_obj_t  *s_images = NULL;
static uint32_t       s_images_cap = 0;
static sg_view_obj_t *s_views = NULL;
static uint32_t       s_views_cap = 0;
static sg_buf_obj_t  *s_buffers = NULL;
static uint32_t       s_buffers_cap = 0;

static bool sg_grow_images(uint32_t need) {
    if (need <= s_images_cap) return true;
    uint32_t new_cap = s_images_cap ? s_images_cap : 16u;
    while (new_cap < need) {
        if (new_cap > UINT32_MAX / 2u) return false;
        new_cap *= 2u;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(sg_img_obj_t)) return false;
    sg_img_obj_t *p = (sg_img_obj_t *)realloc(s_images, (size_t)new_cap * sizeof(sg_img_obj_t));
    if (!p) return false;
    memset(p + s_images_cap, 0, (size_t)(new_cap - s_images_cap) * sizeof(sg_img_obj_t));
    s_images = p;
    s_images_cap = new_cap;
    return true;
}

static bool sg_grow_views(uint32_t need) {
    if (need <= s_views_cap) return true;
    uint32_t new_cap = s_views_cap ? s_views_cap : 16u;
    while (new_cap < need) {
        if (new_cap > UINT32_MAX / 2u) return false;
        new_cap *= 2u;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(sg_view_obj_t)) return false;
    sg_view_obj_t *p = (sg_view_obj_t *)realloc(s_views, (size_t)new_cap * sizeof(sg_view_obj_t));
    if (!p) return false;
    memset(p + s_views_cap, 0, (size_t)(new_cap - s_views_cap) * sizeof(sg_view_obj_t));
    s_views = p;
    s_views_cap = new_cap;
    return true;
}

static bool sg_grow_buffers(uint32_t need) {
    if (need <= s_buffers_cap) return true;
    uint32_t new_cap = s_buffers_cap ? s_buffers_cap : 16u;
    while (new_cap < need) {
        if (new_cap > UINT32_MAX / 2u) return false;
        new_cap *= 2u;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(sg_buf_obj_t)) return false;
    sg_buf_obj_t *p = (sg_buf_obj_t *)realloc(s_buffers, (size_t)new_cap * sizeof(sg_buf_obj_t));
    if (!p) return false;
    memset(p + s_buffers_cap, 0, (size_t)(new_cap - s_buffers_cap) * sizeof(sg_buf_obj_t));
    s_buffers = p;
    s_buffers_cap = new_cap;
    return true;
}

static sg_bindings   s_cur_bindings;
static bool          s_in_offscreen_pass = false;
static uint8_t       s_pac_fb8[PAC_W * PAC_H];
static uint8_t       s_pac_rgb_idx[PAC_W * PAC_H]; /* stores palette index (0..255) for fade */
static uint8_t       s_canvas_fb8[CANVAS_43_W * CANVAS_43_H];
static uint32_t      s_next_handle = 1;

void slog_func(const char *tag, uint32_t log_level, uint32_t log_item_id,
               const char *message_or_null, uint32_t line_nr,
               const char *filename_or_null, void *user_data) {
    (void)log_level;
    (void)log_item_id;
    (void)line_nr;
    (void)filename_or_null;
    (void)user_data;
    if (message_or_null) {
        serial_printf("[pacman/%s] %s\n", tag ? tag : "sokol", message_or_null);
    }
}

void sg_setup(const sg_desc *desc) {
    (void)desc;
    init_wm_palette_lut();
    sg_shutdown();
    memset(s_pac_fb8, 0, sizeof(s_pac_fb8));
    memset(s_canvas_fb8, 0, sizeof(s_canvas_fb8));
}

void sg_shutdown(void) {
    for (uint32_t i = 1; i < s_images_cap; i++) {
        if (s_images[i].used && s_images[i].data) {
            free(s_images[i].data);
            s_images[i].data = NULL;
        }
    }
    free(s_images); s_images = NULL; s_images_cap = 0;
    free(s_views);  s_views = NULL;  s_views_cap = 0;
    free(s_buffers); s_buffers = NULL; s_buffers_cap = 0;
}

sg_backend sg_query_backend(void) {
    return SG_BACKEND_GLCORE;
}

sg_buffer sg_make_buffer(const sg_buffer_desc *desc) {
    for (uint32_t i = 1; i < s_buffers_cap; i++) {
        if (!s_buffers[i].used) {
            s_buffers[i].used = true;
            s_buffers[i].data = desc ? desc->data.ptr : NULL;
            s_buffers[i].size = desc ? desc->size : 0;
            return (sg_buffer){ .id = i };
        }
    }
    uint32_t idx = s_buffers_cap ? s_buffers_cap : 1u;
    if (!sg_grow_buffers(idx + 1u)) return (sg_buffer){ .id = 0 };
    s_buffers[idx].used = true;
    s_buffers[idx].data = desc ? desc->data.ptr : NULL;
    s_buffers[idx].size = desc ? desc->size : 0;
    return (sg_buffer){ .id = idx };
}

sg_image sg_make_image(const sg_image_desc *desc) {
    uint32_t slot = 0;
    for (uint32_t i = 1; i < s_images_cap; i++) {
        if (!s_images[i].used) { slot = i; break; }
    }
    if (slot == 0) {
        slot = s_images_cap ? s_images_cap : 1u;
        if (!sg_grow_images(slot + 1u)) return (sg_image){ .id = 0 };
    }
    sg_img_obj_t *im = &s_images[slot];
    im->used = true;
    im->w = desc ? desc->width : 0;
    im->h = desc ? desc->height : 0;
    im->fmt = desc ? desc->pixel_format : SG_PIXELFORMAT_RGBA8;
    im->is_target = desc ? desc->usage.color_attachment : false;
    im->data = NULL;
    if (desc && desc->data.ptr && desc->data.size > 0) {
        size_t sz = desc->data.size;
        im->data = (uint8_t *)malloc(sz);
        if (im->data) {
            memcpy(im->data, desc->data.ptr, sz);
            if (im->fmt == SG_PIXELFORMAT_RGBA8 && im->h == 1 && im->w <= 256) {
                for (int p = 0; p < im->w; p++) {
                    const uint8_t *rgba = im->data + p * 4;
                    im->pal_wm[p] = rgb_to_wm(rgba[0], rgba[1], rgba[2]);
                }
            }
        }
    }
    return (sg_image){ .id = slot };
}

sg_view sg_make_view(const sg_view_desc *desc) {
    uint32_t slot = 0;
    for (uint32_t i = 1; i < s_views_cap; i++) {
        if (!s_views[i].used) { slot = i; break; }
    }
    if (slot == 0) {
        slot = s_views_cap ? s_views_cap : 1u;
        if (!sg_grow_views(slot + 1u)) return (sg_view){ .id = 0 };
    }
    s_views[slot].used = true;
    if (desc && desc->texture.image.id != 0) {
        s_views[slot].img_id = desc->texture.image.id;
    } else if (desc && desc->color_attachment.image.id != 0) {
        s_views[slot].img_id = desc->color_attachment.image.id;
    } else {
        s_views[slot].img_id = 0;
    }
    return (sg_view){ .id = slot };
}

sg_sampler sg_make_sampler(const sg_sampler_desc *desc) {
    (void)desc;
    return (sg_sampler){ .id = s_next_handle++ };
}

sg_shader sg_make_shader(const sg_shader_desc *desc) {
    (void)desc;
    return (sg_shader){ .id = s_next_handle++ };
}

sg_pipeline sg_make_pipeline(const sg_pipeline_desc *desc) {
    (void)desc;
    return (sg_pipeline){ .id = s_next_handle++ };
}

void sg_write_buffer_transient(const sg_write_buffer_desc *desc) {
    if (!desc) return;
    uint32_t bid = desc->dst.buffer.id;
    if (bid > 0 && bid < s_buffers_cap && s_buffers[bid].used) {
        s_buffers[bid].data = desc->src.data.ptr;
        s_buffers[bid].size = desc->size;
    }
}

void sg_begin_pass(const sg_pass *pass) {
    s_in_offscreen_pass = (pass && pass->attachments.colors[0].id != 0);
    if (s_in_offscreen_pass) {
        memset(s_pac_fb8, 0, sizeof(s_pac_fb8));
        memset(s_pac_rgb_idx, 0, sizeof(s_pac_rgb_idx));
    }
}

void sg_apply_viewport(int x, int y, int width, int height, bool origin_top_left) {
    (void)x; (void)y; (void)width; (void)height; (void)origin_top_left;
}

void sg_apply_pipeline(sg_pipeline pip) {
    (void)pip;
}

void sg_apply_bindings(const sg_bindings *bindings) {
    if (bindings) {
        s_cur_bindings = *bindings;
    } else {
        memset(&s_cur_bindings, 0, sizeof(s_cur_bindings));
    }
}

static void rasterize_pacman_quads(const pac_vertex_t *verts, int num_verts,
                                   const sg_img_obj_t *tilerom, const sg_img_obj_t *pal) {
    if (!verts || !tilerom || !tilerom->data || !pal || !pal->data) return;
    const int tw = tilerom->w;
    const int th = tilerom->h;
    const uint8_t *tiles = tilerom->data;
    const uint8_t *pal_rgba = pal->data;
    const uint8_t *pal_wm = pal->pal_wm;

    for (int vi = 0; vi + 5 < num_verts; vi += 6) {
        const pac_vertex_t *v0 = &verts[vi + 0];
        const pac_vertex_t *v2 = &verts[vi + 2];

        int px0 = (int)(v0->x * (float)PAC_W + 0.5f);
        int py0 = (int)(v0->y * (float)PAC_H + 0.5f);
        int px1 = (int)(v2->x * (float)PAC_W + 0.5f);
        int py1 = (int)(v2->y * (float)PAC_H + 0.5f);

        int tu0 = (int)(v0->u * (float)tw + 0.5f);
        int tv0 = (int)(v0->v * (float)th + 0.5f);
        int tu1 = (int)(v2->u * (float)tw + 0.5f);
        int tv1 = (int)(v2->v * (float)th + 0.5f);

        int sx0 = px0, sx1 = px1;
        bool flip_x = false;
        if (sx0 > sx1) {
            int tmp = sx0; sx0 = sx1; sx1 = tmp;
            flip_x = true;
        }
        int sy0 = py0, sy1 = py1;
        bool flip_y = false;
        if (sy0 > sy1) {
            int tmp = sy0; sy0 = sy1; sy1 = tmp;
            flip_y = true;
        }

        int w = sx1 - sx0;
        int h = sy1 - sy0;
        if (w <= 0 || h <= 0) continue;

        uint8_t color_code = (uint8_t)(v0->attr & 0xFFu);
        uint8_t opacity    = (uint8_t)((v0->attr >> 8) & 0xFFu);
        if (opacity == 0) continue;

        /* Fade overlay fast path: full screen fade quad with opacity < 255 */
        if (opacity < 255 && w >= PAC_W && h >= PAC_H) {
            int inv = 255 - (int)opacity;
            for (int i = 0; i < PAC_W * PAC_H; i++) {
                uint8_t pi = s_pac_rgb_idx[i];
                const uint8_t *c = pal_rgba + ((size_t)pi * 4u);
                uint8_t r = (uint8_t)(((int)c[0] * inv) >> 8);
                uint8_t g = (uint8_t)(((int)c[1] * inv) >> 8);
                uint8_t b = (uint8_t)(((int)c[2] * inv) >> 8);
                s_pac_fb8[i] = rgb_to_wm(r, g, b);
            }
            continue;
        }

        int ud = tu1 - tu0;
        int vd = tv1 - tv0;
        int base_pal = (int)(color_code & 0x3Fu) * 4;
        const uint8_t *bp_rgba = pal_rgba + base_pal * 4;
        const uint8_t *bp_wm   = pal_wm + base_pal;

        /* Fast path: exact 1:1 unflipped 8x8 tile or 16x16 sprite inside bounds */
        if (!flip_x && !flip_y && w == ud && h == vd &&
            sx0 >= 0 && sy0 >= 0 && sx1 <= PAC_W && sy1 <= PAC_H &&
            tu0 >= 0 && tv0 >= 0 && tu1 <= tw && tv1 <= th) {
            for (int dy = 0; dy < h; dy++) {
                const uint8_t *trow = tiles + (size_t)(tv0 + dy) * (size_t)tw + tu0;
                size_t row_off = (size_t)(sy0 + dy) * PAC_W + (size_t)sx0;
                uint8_t *drow = s_pac_fb8 + row_off;
                uint8_t *irow = s_pac_rgb_idx + row_off;
                for (int dx = 0; dx < w; dx++) {
                    uint8_t tc = trow[dx] & 0x03u;
                    if (bp_rgba[tc * 4 + 3] == 0) continue;
                    drow[dx] = bp_wm[tc];
                    irow[dx] = (uint8_t)(base_pal + (int)tc);
                }
            }
            continue;
        }

        /* General clipped / flipped path with per-row X LUT (zero per-pixel divides) */
        int tx_lut[64];
        bool use_lut = (w <= 64);
        if (use_lut) {
            for (int dx = 0; dx < w; dx++) {
                tx_lut[dx] = flip_x ? (tu0 + ((w - 1 - dx) * ud) / w) : (tu0 + (dx * ud) / w);
            }
        }

        for (int dy = 0; dy < h; dy++) {
            int yy = sy0 + dy;
            if ((unsigned)yy >= (unsigned)PAC_H) continue;
            int ty = flip_y ? (tv0 + ((h - 1 - dy) * vd) / h) : (tv0 + (dy * vd) / h);
            if ((unsigned)ty >= (unsigned)th) continue;
            const uint8_t *trow = tiles + (size_t)ty * (size_t)tw;
            uint8_t *drow = s_pac_fb8 + (size_t)yy * PAC_W;
            uint8_t *irow = s_pac_rgb_idx + (size_t)yy * PAC_W;

            for (int dx = 0; dx < w; dx++) {
                int xx = sx0 + dx;
                if ((unsigned)xx >= (unsigned)PAC_W) continue;
                int tx = use_lut ? tx_lut[dx] : (flip_x ? (tu0 + ((w - 1 - dx) * ud) / w) : (tu0 + (dx * ud) / w));
                if ((unsigned)tx >= (unsigned)tw) continue;

                uint8_t tc = trow[tx] & 0x03u;
                if (bp_rgba[tc * 4 + 3] == 0) continue;
                drow[xx] = bp_wm[tc];
                irow[xx] = (uint8_t)(base_pal + (int)tc);
            }
        }
    }
}

void sg_draw(int base_element, int num_elements, int num_instances) {
    (void)num_instances;
    if (!s_in_offscreen_pass) return;
    uint32_t bid = s_cur_bindings.vertex_buffers[0].id;
    uint32_t v0 = s_cur_bindings.views[0].id;
    uint32_t v1 = s_cur_bindings.views[1].id;
    if (bid == 0 || bid >= s_buffers_cap || !s_buffers[bid].used) return;
    if (v0 == 0 || v0 >= s_views_cap || !s_views[v0].used) return;
    if (v1 == 0 || v1 >= s_views_cap || !s_views[v1].used) return;

    uint32_t i0 = s_views[v0].img_id;
    uint32_t i1 = s_views[v1].img_id;
    if (i0 == 0 || i0 >= s_images_cap || !s_images[i0].used) return;
    if (i1 == 0 || i1 >= s_images_cap || !s_images[i1].used) return;

    sg_img_obj_t *tilerom = &s_images[i0];
    sg_img_obj_t *pal     = &s_images[i1];
    const pac_vertex_t *verts = (const pac_vertex_t *)s_buffers[bid].data;
    if (!verts) return;
    rasterize_pacman_quads(verts + base_element, num_elements, tilerom, pal);

    static uint32_t s_dbg_draws = 0;
    (void)s_dbg_draws;
}

void sg_end_pass(void) {
    s_in_offscreen_pass = false;
}

void sg_commit(void) {
    /* Present is handled in the app frame loop */
}

sg_environment sglue_environment(void) {
    sg_environment env;
    memset(&env, 0, sizeof(env));
    return env;
}

sg_swapchain sglue_swapchain(void) {
    sg_swapchain sc;
    memset(&sc, 0, sizeof(sc));
    return sc;
}

/* =========================================================================
 * Sokol Audio -> IPO_OS SB16 Ring Buffer (22050 Hz S16 Fast-Path)
 * ========================================================================= */
#define AUDIO_RING_SAMPLES 8192

static bool            s_audio_ok = false;
static audio_stream_t *s_audio_stream = NULL;
static int16_t         s_audio_ring[AUDIO_RING_SAMPLES];
static size_t          s_audio_write_pos = 0;

void saudio_setup(const saudio_desc *desc) {
    (void)desc;
    memset(s_audio_ring, 0, sizeof(s_audio_ring));
    if (audio_init()) {
        s_audio_stream = audio_stream_create_ring(AUDIO_FORMAT_S16, 1, 22050u,
                                                  s_audio_ring, AUDIO_RING_SAMPLES, 0u);
        if (s_audio_stream) {
            audio_stream_play(s_audio_stream);
            s_audio_ok = true;
        }
    }
}

void saudio_shutdown(void) {
    if (s_audio_stream) {
        audio_stream_stop(s_audio_stream);
        audio_stream_destroy(s_audio_stream);
        s_audio_stream = NULL;
    }
    s_audio_ok = false;
}

int saudio_sample_rate(void) {
    return 22050;
}

int saudio_push(const float *frames, int num_frames) {
    if (!s_audio_ok || !s_audio_stream || !frames || num_frames <= 0) return num_frames;
    ipo_audio_clock_t clk;
    memset(&clk, 0, sizeof(clk));
    clk.struct_size = (uint32_t)sizeof(clk);
    int push_n = num_frames;
    if (ipo_audio_get_stream_clock(s_audio_stream, &clk) == IPO_AUDIO_OK) {
        uint64_t ahead = (s_audio_write_pos > clk.played_frames) ? (s_audio_write_pos - clk.played_frames) : 0ULL;
        if (ahead >= AUDIO_RING_SAMPLES - 256u) {
            audio_tick();
            return 0;
        }
        int room = (int)((AUDIO_RING_SAMPLES - 256u) - ahead);
        if (push_n > room) push_n = room;
    }
    for (int i = 0; i < push_n; i++) {
        float v = frames[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        s_audio_ring[s_audio_write_pos % AUDIO_RING_SAMPLES] = (int16_t)(v * 32000.0f);
        s_audio_write_pos++;
    }
    audio_stream_update_buffered(s_audio_stream, s_audio_write_pos, false);
    audio_tick();
    return push_n;
}

/* =========================================================================
 * Sokol App -> IPO_OS Window Manager / Mode 13h Video & Input
 * ========================================================================= */
static sapp_desc    s_app_desc;
static wm_window_t *s_wm_win = NULL;
static bool         s_direct_gfx = false;
static bool         s_fullscreen = false;
static bool         s_should_quit = false;
static uint32_t     s_frame_dur_ms = 16;
static int          s_win_w = CANVAS_43_W;
static int          s_win_h = CANVAS_43_H;
static ipo_hd_view_t s_hd;

double sapp_frame_duration(void) {
    return (double)s_frame_dur_ms / 1000.0;
}

int sapp_width(void) {
    return s_win_w;
}

int sapp_height(void) {
    return s_win_h;
}

void sapp_toggle_fullscreen(void) {
    if (s_wm_win && wm_is_window_valid(s_wm_win)) {
        s_fullscreen = !s_fullscreen;
        wm_set_fullscreen(s_wm_win, s_fullscreen);
        wm_set_cursor_visible(s_wm_win, !s_fullscreen);
    }
}

static sapp_keycode ps2_to_sapp_key(uint8_t sc, bool e0) {
    if (e0) {
        switch (sc) {
            case 0x48: return SAPP_KEYCODE_UP;
            case 0x50: return SAPP_KEYCODE_DOWN;
            case 0x4B: return SAPP_KEYCODE_LEFT;
            case 0x4D: return SAPP_KEYCODE_RIGHT;
            default:   return SAPP_KEYCODE_INVALID;
        }
    }
    switch (sc) {
        case 0x01: return SAPP_KEYCODE_ESCAPE;
        case 0x11: return SAPP_KEYCODE_W;
        case 0x1E: return SAPP_KEYCODE_A;
        case 0x1F: return SAPP_KEYCODE_S;
        case 0x20: return SAPP_KEYCODE_D;
        case 0x21: return SAPP_KEYCODE_F;
        case 0x1C: return SAPP_KEYCODE_ENTER;
        case 0x39: return SAPP_KEYCODE_SPACE;
        default:   return SAPP_KEYCODE_SPACE; /* triggers anykey on intro */
    }
}

static void emit_key_event(sapp_keycode kc, bool down) {
    if (kc == SAPP_KEYCODE_INVALID || !s_app_desc.event_cb) return;
    if (down && kc == SAPP_KEYCODE_ESCAPE) {
        s_should_quit = true;
    }
    sapp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? SAPP_EVENTTYPE_KEY_DOWN : SAPP_EVENTTYPE_KEY_UP;
    ev.key_code = kc;
    s_app_desc.event_cb(&ev);
}

static void poll_input_events(void) {
    if (s_wm_win) {
        if (!wm_is_window_valid(s_wm_win) || !wm_session_active()) {
            s_should_quit = true;
            return;
        }
        wm_event_t ev;
        while (wm_poll_event(s_wm_win, &ev)) {
            if (ev.type == WM_EVENT_CLOSE) {
                s_should_quit = true;
                return;
            }
            if (ev.type == WM_EVENT_KEY_EXT) {
                bool rel = (ev.data & 0x80000000u) != 0u;
                uint32_t kc = ev.data & 0x7FFFFFFFu;
                bool e0 = ((kc & 0xFF00u) == 0xE000u);
                uint8_t sc = (uint8_t)(kc & 0x7Fu);
                emit_key_event(ps2_to_sapp_key(sc, e0), !rel);
            } else if (ev.type == WM_EVENT_KEY_DOWN || ev.type == WM_EVENT_KEY_UP) {
                bool down = (ev.type == WM_EVENT_KEY_DOWN);
                static bool s_e0 = false;
                uint8_t raw = (uint8_t)(ev.data & 0xFFu);
                if (raw == 0xE0u) { s_e0 = true; continue; }
                bool is_rel = (raw & 0x80u) != 0u;
                uint8_t sc = raw & 0x7Fu;
                if (sc != 0) {
                    emit_key_event(ps2_to_sapp_key(sc, s_e0), down && !is_rel);
                }
                s_e0 = false;
            }
        }
    } else if (s_direct_gfx) {
        static bool s_e0 = false;
        uint8_t raw;
        keyboard_poll();
        while ((raw = keyboard_get_scancode()) != 0u) {
            if (raw == 0xE0u) { s_e0 = true; continue; }
            bool rel = (raw & 0x80u) != 0u;
            uint8_t sc = raw & 0x7Fu;
            emit_key_event(ps2_to_sapp_key(sc, s_e0), !rel);
            s_e0 = false;
        }
    }
}

static void present_pacman_frame(void) {
    /* Present 224x288 playfield directly at 2x (448x576) inside 800x600 when available */
    init_wm_palette_lut();
    if (!ipo_hd_present_pal32(&s_hd, s_wm_win, s_direct_gfx, s_pac_fb8, PAC_W, PAC_H, PAC_W, s_wm_pal32)) {
        ipo_hd_present(&s_hd, s_wm_win, s_direct_gfx, s_pac_fb8, PAC_W, PAC_H, PAC_W);
    }
}

int main(int argc, char *argv[]) {
    s_app_desc = sokol_main(argc, argv);

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    {
        wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
        opt.title = s_app_desc.window_title ? s_app_desc.window_title : "pacman.c";
        opt.bg_color = 0;
        opt.fullscreen = true;
        opt.w = (uint16_t)(sw > 320 ? sw : 320);
        opt.h = (uint16_t)(sh > 200 ? sh : 200);
        opt.x = 0;
        opt.y = 0;
        s_wm_win = wm_create_window(&opt);
    }

    if (s_wm_win) {
        s_fullscreen = true;
        wm_set_fullscreen(s_wm_win, true);
        wm_set_cursor_visible(s_wm_win, false);
        wm_compositor_tick();
    } else {
        vga_set_mode_13h();
        apply_wm_dac_palette();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        s_direct_gfx = true;
    }
    memset(&s_hd, 0, sizeof(s_hd));
    ipo_hd_setup(&s_hd, s_wm_win, PAC_W, PAC_H, true);
    s_win_w = s_hd.scr_w;
    s_win_h = s_hd.scr_h;

    if (s_app_desc.init_cb) s_app_desc.init_cb();

    uint32_t fps_t0 = timer_millis();
    uint32_t fps_frames = 0;
    uint32_t last_ms = fps_t0;
    uint32_t next_frame_ms = last_ms;

    while (!s_should_quit) {
        if (s_direct_gfx && system_is_interrupted()) {
            system_clear_interrupt();
            break;
        }
        uint32_t now = timer_millis();
        if (now < next_frame_ms) {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
            continue;
        }

        uint32_t dt = now - last_ms;
        if (dt < 1u) dt = 16u;
        if (dt > 33u) dt = 33u;
        s_frame_dur_ms = dt;
        last_ms = now;
        next_frame_ms += 16u; /* 60 FPS target */
        if ((int32_t)(now - next_frame_ms) > 64) {
            next_frame_ms = now + 16u;
        }

        poll_input_events();
        if (s_should_quit) break;

        if (s_app_desc.frame_cb) s_app_desc.frame_cb();
        present_pacman_frame();

        fps_frames++;
        uint32_t elapsed = now - fps_t0;
        if (elapsed >= 1000u) {
            uint32_t fps = (fps_frames * 1000u) / elapsed;
            serial_printf("[pacman] %u FPS\n", fps);
            fps_frames = 0;
            fps_t0 = now;
        }
    }

    if (s_app_desc.cleanup_cb) s_app_desc.cleanup_cb();
    ipo_hd_free(&s_hd);

    if (s_wm_win && wm_is_window_valid(s_wm_win)) {
        wm_set_cursor_visible(s_wm_win, true);
        wm_destroy_window(s_wm_win);
        s_wm_win = NULL;
    } else if (s_direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
    }
    return 0;
}
