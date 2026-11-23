/*
 * gl_teapot.c - Photorealistic Studio 3D Utah Teapot Renderer
 *
 * High-speed fixed-point deferred PBR pipeline:
 * - High-detail connected Utah Teapot mesh (3,644 vertices, 6,320 triangles)
 * - Real 3D solid-angle self-occlusion baked per-vertex (cavity & contact AO)
 * - Smooth 64-segment beveled obsidian/slate turntable plinth with satin reflection
 * - 100% integer 16.16 fixed-point G-buffer rasterizer (zero per-pixel floats)
 * - Two-pass 5x5 Gaussian projected soft shadow with grounded contact penumbra
 * - 6x32x32 Studio Light-Tent HDR Cubemap (curved softboxes + negative-fill baffles)
 * - Precomputed Trowbridge-Reitz GGX specular + Schlick Fresnel + ACES Filmic LUT
 * - Floyd-Steinberg serpentine studio cyclorama + low-noise Bayer RGB555 quantizer
 * - Windowed mode + crisp F11 2x2 HD fullscreen (400x300 -> 800x600)
 */

#include <GL/gl.h>
#include <GL/glu.h>
#include <GL/ipo_gl.h>
#include <GL/gl_math.h>
#include <driver/color.h>
#include <ipo/gfx.h>
#include <ipo/gfx3d.h>
#include <ipo/gpu_client.h>
#include <wm.h>
#include <vga_gfx.h>
#include <math.h>
#include <syscall.h>
#include <threads.h>
#include <system/timer.h>
#include <system/state.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/ipo_hd_present.h"
#include "teapot_data.h"

#define DEFAULT_WIN_W 272
#define DEFAULT_WIN_H 164

static char *tp_alloc_fmt(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (len < 0) {
        va_end(ap2);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)len + 1u);
    if (!buf) {
        va_end(ap2);
        return NULL;
    }
    vsnprintf(buf, (size_t)len + 1u, fmt, ap2);
    va_end(ap2);
    return buf;
}

#define SC_ESC          0x01
#define SC_Q            0x10
#define SC_R            0x13
#define SC_S            0x1F
#define SC_L            0x26
#define SC_M            0x32
#define SC_SPACE        0x39
#define SC_UP           0x48
#define SC_PAGE_UP      0x49
#define SC_KEYPAD_MINUS 0x4A
#define SC_LEFT         0x4B
#define SC_RIGHT        0x4D
#define SC_KEYPAD_PLUS  0x4E
#define SC_DOWN         0x50
#define SC_PAGE_DOWN    0x51

typedef struct {
    const char *name;
    float albedo[3];
    float f0[3];
    float metallic;
    float roughness;
    float env_strength;
    float clearcoat;
} pbr_material_t;

static const pbr_material_t materials[] = {
    {
        .name = "Ming Crimson Glaze",
        .albedo = { 0.74f, 0.048f, 0.040f },
        .f0 = { 0.055f, 0.055f, 0.055f },
        .metallic = 0.0f,
        .roughness = 0.16f,
        .env_strength = 1.05f,
        .clearcoat = 0.85f
    },
    {
        .name = "Polished 24K Gold",
        .albedo = { 0.98f, 0.76f, 0.25f },
        .f0 = { 0.98f, 0.78f, 0.30f },
        .metallic = 0.98f,
        .roughness = 0.20f,
        .env_strength = 1.15f,
        .clearcoat = 0.0f
    },
    {
        .name = "Mirror Chrome",
        .albedo = { 0.90f, 0.92f, 0.96f },
        .f0 = { 0.94f, 0.95f, 0.98f },
        .metallic = 1.00f,
        .roughness = 0.12f,
        .env_strength = 1.25f,
        .clearcoat = 0.0f
    },
    {
        .name = "Imperial Jade Porcelain",
        .albedo = { 0.055f, 0.48f, 0.20f },
        .f0 = { 0.055f, 0.060f, 0.055f },
        .metallic = 0.0f,
        .roughness = 0.18f,
        .env_strength = 1.00f,
        .clearcoat = 0.80f
    },
    {
        .name = "Royal Cobalt Ceramic",
        .albedo = { 0.06f, 0.15f, 0.74f },
        .f0 = { 0.055f, 0.055f, 0.062f },
        .metallic = 0.0f,
        .roughness = 0.16f,
        .env_strength = 1.05f,
        .clearcoat = 0.85f
    },
    {
        .name = "Burnished Copper",
        .albedo = { 0.94f, 0.43f, 0.24f },
        .f0 = { 0.95f, 0.49f, 0.31f },
        .metallic = 0.95f,
        .roughness = 0.22f,
        .env_strength = 1.10f,
        .clearcoat = 0.0f
    }
};

#define NUM_MATERIALS ((int)(sizeof(materials) / sizeof(materials[0])))

static volatile bool app_running = true;
static float rot_x = 18.0f;
static float rot_y = -28.0f;
static float cam_dist = 5.4f;
static bool auto_spin = true;
static int current_mat = 0;
static bool shadows_enabled = true;
static bool light1_enabled = true;

/* =========================================================================
 * Precomputed Fixed-Point Tables & Perceptual 248-Color WM Quantizer LUT
 * ========================================================================= */
#define HDR_Q8_MAX   1024   /* 1024 = 4.0f linear HDR in Q8 */
#define CUBE_SZ      64
#define CUBE_UV_MAX  ((CUBE_SZ - 1) * 256 - 1)
#define INV_LEN_SLOTS 3072  /* Covers len2 up to 127^2 * 3 = 48387 (>> 4) */

static uint8_t  s_aces_lut[HDR_Q8_MAX];
static uint8_t  s_srgb_lut[257];
static uint8_t  s_rgb555_wm_lut[32768];
static bool     s_wm8_lut_ready = false;
static uint8_t  s_wm_palette[248][3];
static uint16_t s_rcp_q15[513];
static uint32_t s_inv_len_q16[INV_LEN_SLOTS];
static uint16_t s_spec_ggx_lut[NUM_MATERIALS][257];
static uint16_t s_ped_spec_lut[257];
static uint16_t s_env_cube[6][CUBE_SZ][CUBE_SZ][3]; /* Q8 HDR RGB (8bpp fallback) */
static int16_t  s_env_cube_q12[6][CUBE_SZ][CUBE_SZ][4]; /* 16-byte aligned Q12 HDR cubemap */
static uint16_t s_rsqrt_q20[4096];
static uint16_t s_cube_scale_q12[2049];
static uint16_t s_fresnel_q12[1025];
static uint32_t s_ggx_q12[NUM_MATERIALS][1025];
static uint16_t s_smith_g_q12[NUM_MATERIALS][1025];
static uint16_t s_cc_q12[1025];
static uint16_t s_ped_amb_q12[256][3];
static uint16_t s_ped_dir_q12[256][3];
static int8_t   s_ign_tile[64][64];
static double   s_q12_to_f64[32769];
static bool     s_tables_ready = false;
static ipo_gfx_caps_t s_gfx_caps;
static bool     s_caps_ready = false;

static inline double q12_to_f64_fast(int32_t v) {
    if (v <= 0) return 0.0;
    if (v <= 32768) return s_q12_to_f64[v];
    return (double)v * (1.0 / 4096.0);
}

typedef void (*tp_job_fn_t)(int tid, void *ctx);

typedef struct {
    ipo_thread_t *threads;
    int worker_count;
    volatile uint32_t epoch;
    volatile uint32_t active_done;
    volatile int stop;
    tp_job_fn_t fn;
    void *ctx;
} tp_pool_t;

static tp_pool_t s_pool = {0};
static volatile int s_tp_hot = 0;

static void *teapot_worker_entry(void *arg) {
    int tid = (int)(uintptr_t)arg;
    uint32_t last_epoch = 0u;
    for (;;) {
        if (__atomic_load_n(&s_pool.stop, __ATOMIC_ACQUIRE)) {
            break;
        }
        uint32_t ep = __atomic_load_n(&s_pool.epoch, __ATOMIC_ACQUIRE);
        if (ep == last_epoch) {
            uint32_t budget = __atomic_load_n(&s_tp_hot, __ATOMIC_RELAXED) ? 250000u : 256u;
            uint32_t s = 0u;
            while (s < budget) {
                __asm__ volatile("pause" ::: "memory");
                ep = __atomic_load_n(&s_pool.epoch, __ATOMIC_ACQUIRE);
                if (ep != last_epoch || __atomic_load_n(&s_pool.stop, __ATOMIC_ACQUIRE)) {
                    break;
                }
                s++;
            }
            if (__atomic_load_n(&s_pool.stop, __ATOMIC_ACQUIRE)) break;
            if (ep == last_epoch) {
                ipo_thread_yield();
                continue;
            }
        }
        last_epoch = ep;
        tp_job_fn_t fn = s_pool.fn;
        void *ctx = s_pool.ctx;
        if (fn) {
            fn(tid, ctx);
        }
        __atomic_fetch_add(&s_pool.active_done, 1u, __ATOMIC_ACQ_REL);
    }
    return NULL;
}

static void tp_init(void) {
    if (s_pool.worker_count > 0) return;
    uint32_t active_cpus = 0u;
    uint32_t sc_args[1] = { (uint32_t)(uintptr_t)&active_cpus };
    uint32_t cpus = (uint32_t)ipo_syscall(IPO_SYSCALL_SMP_GET_COUNT, 1u, sc_args);
    if (active_cpus > 1u && active_cpus < cpus) cpus = active_cpus;
    if (cpus <= 1u) return;
    uint32_t want_u = cpus - 1u;
    if ((size_t)want_u > SIZE_MAX / sizeof(ipo_thread_t)) return;
    ipo_thread_t *threads = (ipo_thread_t *)calloc((size_t)want_u, sizeof(ipo_thread_t));
    if (!threads) return;
    s_pool.threads = threads;
    __atomic_store_n(&s_pool.stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pool.epoch, 0u, __ATOMIC_RELEASE);
    int created = 0;
    for (uint32_t i = 0; i < want_u; i++) {
        if (ipo_thread_create(&s_pool.threads[i],
                              teapot_worker_entry,
                              (void *)(uintptr_t)(i + 1u)) == 0) {
            created++;
        } else {
            break;
        }
    }
    if (created <= 0) {
        free(s_pool.threads);
        s_pool.threads = NULL;
        return;
    }
    s_pool.worker_count = created;
}

static void tp_dispatch(tp_job_fn_t fn, void *ctx) {
    if (s_pool.worker_count <= 0) {
        fn(0, ctx);
        return;
    }
    s_pool.fn = fn;
    s_pool.ctx = ctx;
    __atomic_store_n(&s_pool.active_done, 0u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s_pool.epoch, 1u, __ATOMIC_ACQ_REL);
    fn(0, ctx);
    uint32_t need = (uint32_t)s_pool.worker_count;
    uint32_t spin = 0u;
    while (__atomic_load_n(&s_pool.active_done, __ATOMIC_ACQUIRE) < need) {
        if (++spin > 250000u) {
            ipo_thread_yield();
            spin = 0u;
        } else {
            __asm__ volatile("pause" ::: "memory");
        }
    }
}

static void tp_shutdown(void) {
    if (s_pool.worker_count <= 0) {
        free(s_pool.threads);
        s_pool.threads = NULL;
        return;
    }
    __atomic_store_n(&s_pool.stop, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s_pool.epoch, 1u, __ATOMIC_ACQ_REL);
    for (int i = 0; i < s_pool.worker_count; i++) {
        ipo_thread_join(s_pool.threads[i], NULL);
    }
    free(s_pool.threads);
    s_pool.threads = NULL;
    s_pool.worker_count = 0;
}

static void init_cube_face_dir(int face, float u, float v, float out[3]) {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    switch (face) {
        case 0: x =  1.0f; y = -v;    z = -u;    break; /* +X */
        case 1: x = -1.0f; y = -v;    z =  u;    break; /* -X */
        case 2: x =  u;    y =  1.0f; z =  v;    break; /* +Y */
        case 3: x =  u;    y = -1.0f; z = -v;    break; /* -Y */
        case 4: x =  u;    y = -v;    z =  1.0f; break; /* +Z */
        default:x = -u;    y = -v;    z = -1.0f; break; /* -Z */
    }
    float inv = 1.0f / gl_sqrtf(x * x + y * y + z * z);
    out[0] = x * inv;
    out[1] = y * inv;
    out[2] = z * inv;
}

static void init_studio_cubemap(void) {
    /* Smooth light-tent HDR cubemap with studio softboxes & contrast baffles */
    const float inv_span = 1.0f / (float)(CUBE_SZ - 1);
    for (int f = 0; f < 6; f++) {
        for (int iy = 0; iy < CUBE_SZ; iy++) {
            float v = ((float)iy * inv_span) * 2.0f - 1.0f;
            for (int ix = 0; ix < CUBE_SZ; ix++) {
                float u = ((float)ix * inv_span) * 2.0f - 1.0f;
                float d[3];
                init_cube_face_dir(f, u, v, d);
                float rx = d[0], ry = d[1], rz = d[2];

                /* Dark studio cyclorama dome + warm ground bounce */
                float r = 0.032f, g = 0.035f, b = 0.046f;
                if (ry >= 0.0f) {
                    float dome = 0.045f + 0.19f * (1.0f - ry) * (1.0f - ry);
                    r += dome * 0.86f;
                    g += dome * 0.92f;
                    b += dome * 1.10f;
                } else {
                    float abs_y = -ry;
                    float ring = (1.0f - abs_y);
                    float flr = 0.03f + 0.16f * ring * ring;
                    r += flr * 1.05f;
                    g += flr * 0.96f;
                    b += flr * 0.85f;
                }

                /* Crisp studio horizon reflector strip */
                float abs_ry = (ry >= 0.0f) ? ry : -ry;
                float h_band = 1.0f - gl_minf(1.0f, abs_ry * 5.2f);
                float h_glow = h_band * h_band * 0.28f;
                r += h_glow * 0.96f;
                g += h_glow * 0.98f;
                b += h_glow * 1.05f;

                /* 1. Large curved key overhead-right octabox with luminous inner diff & dark skirt */
                {
                    float dx = (rx - 0.50f) / 0.30f;
                    float dy = (ry - 0.54f) / 0.27f;
                    float dz = (rz - 0.36f) / 0.36f;
                    float dist2 = dx * dx + dy * dy + dz * dz;
                    if (dist2 < 1.15f) {
                        float t = 1.0f - dist2 / 1.15f;
                        float soft = t * t * (3.0f - 2.0f * t);
                        float core = (dist2 < 0.35f) ? (1.20f + 0.55f * (1.0f - dist2 / 0.35f)) : 1.0f;
                        float bar = (fabsf(rx - 0.50f) < 0.032f || fabsf(rz - 0.38f) < 0.032f) ? 0.24f : 1.0f;
                        r += soft * 3.35f * core * bar;
                        g += soft * 3.12f * core * bar;
                        b += soft * 2.72f * core * bar;
                    }
                }

                /* 2. Crisp cool vertical strip box on upper-left */
                {
                    float dx = (rx + 0.65f) / 0.17f;
                    float dy = (ry - 0.28f) / 0.44f;
                    float dz = (rz - 0.08f) / 0.40f;
                    float dist2 = dx * dx + dy * dy + dz * dz;
                    if (dist2 < 1.05f) {
                        float t = 1.0f - dist2 / 1.05f;
                        float soft = t * t * (3.0f - 2.0f * t);
                        r += soft * 1.45f;
                        g += soft * 1.78f;
                        b += soft * 2.38f;
                    }
                }

                /* 3. Back overhead hair/rim softbox highlight */
                {
                    float dx = (rx - 0.02f) / 0.42f;
                    float dy = (ry - 0.74f) / 0.20f;
                    float dz = (rz + 0.58f) / 0.30f;
                    float dist2 = dx * dx + dy * dy + dz * dz;
                    if (dist2 < 1.05f) {
                        float t = 1.0f - dist2 / 1.05f;
                        float soft = t * t * (fabsf(rx) < 0.035f ? 0.28f : 1.0f);
                        r += soft * 1.78f;
                        g += soft * 1.82f;
                        b += soft * 1.98f;
                    }
                }

                /* 4. Crisp negative-fill black velvet baffle for rich studio contrast */
                if (rz > 0.34f) {
                    float dx = (rx - 0.01f) / 0.34f;
                    float dy = (ry + 0.03f) / 0.42f;
                    float d2 = dx * dx + dy * dy;
                    if (d2 < 1.0f) {
                        float t = 1.0f - d2;
                        float fade = 1.0f - 0.68f * t * t;
                        r *= fade;
                        g *= fade;
                        b *= fade;
                    }
                }

                int ir = (int)(r * 256.0f + 0.5f);
                int ig = (int)(g * 256.0f + 0.5f);
                int ib = (int)(b * 256.0f + 0.5f);
                if (ir < 0) ir = 0; else if (ir > 1023) ir = 1023;
                if (ig < 0) ig = 0; else if (ig > 1023) ig = 1023;
                if (ib < 0) ib = 0; else if (ib > 1023) ib = 1023;
                s_env_cube[f][iy][ix][0] = (uint16_t)ir;
                s_env_cube[f][iy][ix][1] = (uint16_t)ig;
                s_env_cube[f][iy][ix][2] = (uint16_t)ib;
                int ir12 = (int)(r * 4096.0f + 0.5f);
                int ig12 = (int)(g * 4096.0f + 0.5f);
                int ib12 = (int)(b * 4096.0f + 0.5f);
                if (ir12 < 0) ir12 = 0; else if (ir12 > 32767) ir12 = 32767;
                if (ig12 < 0) ig12 = 0; else if (ig12 > 32767) ig12 = 32767;
                if (ib12 < 0) ib12 = 0; else if (ib12 > 32767) ib12 = 32767;
                s_env_cube_q12[f][iy][ix][0] = (int16_t)ir12;
                s_env_cube_q12[f][iy][ix][1] = (int16_t)ig12;
                s_env_cube_q12[f][iy][ix][2] = (int16_t)ib12;
                s_env_cube_q12[f][iy][ix][3] = 4096;
            }
        }
    }
}

static void ensure_wm8_lut(void) {
    if (s_wm8_lut_ready) return;
    static const uint8_t s_comp_q6[32] = {
        0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2,
        3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5
    };
    for (int r5 = 0; r5 < 32; r5++) {
        int r = (r5 * 255) / 31;
        int cr = (int)s_comp_q6[r5];
        int dr_c = r - cr * 51;
        uint32_t dr2 = (uint32_t)(dr_c * dr_c * 3);
        for (int g5 = 0; g5 < 32; g5++) {
            int g = (g5 * 255) / 31;
            int cg = (int)s_comp_q6[g5];
            int dg_c = g - cg * 51;
            uint32_t dg2 = (uint32_t)(dg_c * dg_c * 4);
            for (int b5 = 0; b5 < 32; b5++) {
                int b = (b5 * 255) / 31;
                int cb = (int)s_comp_q6[b5];
                int db_c = b - cb * 51;
                uint32_t best_d = dr2 + dg2 + (uint32_t)(db_c * db_c * 2);
                int best = 32 + cr * 36 + cg * 6 + cb;

                int span_max = (r > g ? (r > b ? r : b) : (g > b ? g : b));
                int span_min = (r < g ? (r < b ? r : b) : (g < b ? g : b));
                if (span_max - span_min <= 42) {
                    for (int i = 16; i < 32; i++) {
                        int dr = r - (int)s_wm_palette[i][0];
                        int dg = g - (int)s_wm_palette[i][1];
                        int db = b - (int)s_wm_palette[i][2];
                        uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
                        if (d < best_d) {
                            best_d = d;
                            best = i;
                        }
                    }
                }
                s_rgb555_wm_lut[(r5 << 10) | (g5 << 5) | b5] = (uint8_t)best;
            }
        }
    }
    s_wm8_lut_ready = true;
}

static void init_tables(void) {
    if (s_tables_ready) return;

    /* 1. ACES Filmic + Gamma 2.2 LUT over Q8 [0 .. 4.0) */
    for (int i = 0; i < HDR_Q8_MAX; i++) {
        float x = (float)i / 256.0f;
        float t = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        int v = (int)(gl_sqrtf(t) * 255.0f + 0.5f);
        if (v > 255) v = 255;
        s_aces_lut[i] = (uint8_t)v;
    }

    /* 2. Gamma LUT for [0 .. 256] */
    for (int i = 0; i <= 256; i++) {
        float x = (float)i / 256.0f;
        int v = (int)(gl_sqrtf(x) * 255.0f + 0.5f);
        if (v > 255) v = 255;
        s_srgb_lut[i] = (uint8_t)v;
    }

    /* 3. Fast integer reciprocals & inverse-length LUTs */
    s_rcp_q15[0] = 32767;
    for (int i = 1; i <= 512; i++) {
        s_rcp_q15[i] = (uint16_t)(32768u / (uint32_t)i);
    }

    s_inv_len_q16[0] = 65536u;
    for (int i = 1; i < INV_LEN_SLOTS; i++) {
        float l2 = (float)(i << 4) + 8.0f;
        float val = (256.0f * 65536.0f) / gl_sqrtf(l2);
        if (val > 4000000.0f) val = 4000000.0f;
        s_inv_len_q16[i] = (uint32_t)(val + 0.5f);
    }

    /* 4. Trowbridge-Reitz GGX Microfacet NDF LUT per material + Pedestal */
    for (int m = 0; m < NUM_MATERIALS; m++) {
        float rough = materials[m].roughness;
        float a = rough * rough;
        float a2 = a * a;
        float clear_a = 0.075f * 0.075f;
        float clear_a2 = clear_a * clear_a;
        for (int i = 0; i <= 256; i++) {
            float ndoth = (float)i / 256.0f;
            float d_denom = (ndoth * ndoth * (a2 - 1.0f) + 1.0f);
            float ggx = a2 / (3.14159265f * d_denom * d_denom + 1e-5f);
            float spec = ggx * 0.055f;
            if (materials[m].clearcoat > 0.0f) {
                float c_denom = (ndoth * ndoth * (clear_a2 - 1.0f) + 1.0f);
                float c_ggx = clear_a2 / (3.14159265f * c_denom * c_denom + 1e-5f);
                spec += c_ggx * 0.018f * materials[m].clearcoat;
            }
            if (ndoth < 0.25f) spec *= (ndoth * 4.0f);
            int q = (int)(spec * 256.0f + 0.5f);
            if (q > 820) q = 820;
            s_spec_ggx_lut[m][i] = (uint16_t)q;
        }
    }
    for (int i = 0; i <= 256; i++) {
        float ndoth = (float)i / 256.0f;
        float p_a2 = 0.14f * 0.14f;
        float denom = ndoth * ndoth * (p_a2 - 1.0f) + 1.0f;
        float spec = (p_a2 / (3.14159265f * denom * denom + 1e-5f)) * 0.016f;
        if (ndoth < 0.35f) spec = 0.0f;
        int q = (int)(spec * 256.0f + 0.5f);
        if (q > 256) q = 256;
        s_ped_spec_lut[i] = (uint16_t)q;
    }

    /* 5. Exact 248-entry WM palette + fast separable RGB555 LUT */
    static const uint8_t cga[16][3] = {
        {  0,   0,   0}, {  0,   0, 170}, {  0, 170,   0}, {  0, 170, 170},
        {170,   0,   0}, {170,   0, 170}, {170,  85,   0}, {170, 170, 170},
        { 85,  85,  85}, { 85,  85, 255}, { 85, 255,  85}, { 85, 255, 255},
        {255,  85,  85}, {255,  85, 255}, {255, 255,  85}, {255, 255, 255}
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

    init_studio_cubemap();

    /* 6. Compact L1-friendly integer Q12/Q14 PBR LUTs for zero-float RGBA64F shading */
    s_rsqrt_q20[0] = 65535u;
    for (int i = 1; i < 4096; i++) {
        uint32_t v = (uint32_t)(1048576.0f / gl_sqrtf((float)i) + 0.5f);
        if (v > 65535u) v = 65535u;
        s_rsqrt_q20[i] = (uint16_t)v;
    }

    s_cube_scale_q12[0] = 65535u;
    for (int b = 1; b <= 2048; b++) {
        uint32_t m_mid = (uint32_t)((b << 3) + 4);
        if (m_mid > 16385u) m_mid = 16385u;
        uint32_t sc = (uint32_t)(((double)(CUBE_SZ - 1) * 128.0 * 4096.0) / (double)m_mid + 0.5);
        if (sc > 65535u) sc = 65535u;
        s_cube_scale_q12[b] = (uint16_t)sc;
    }

    const double clear_a = 0.075 * 0.075;
    const double clear_a2 = clear_a * clear_a;
    const double inv_pi = 0.31830988618;
    for (int i = 0; i <= 1024; i++) {
        double t = (double)i * (1.0 / 1024.0);
        double om = 1.0 - t;
        double om2 = om * om;
        double f5 = om2 * om2 * om;
        s_fresnel_q12[i] = (uint16_t)(f5 * 4096.0 + 0.5);

        double c_denom = t * t * (clear_a2 - 1.0) + 1.0;
        double cd = c_denom * c_denom + 1e-7;
        double cc = (clear_a2 * inv_pi * 0.020) / cd;
        if (t < 0.25) cc *= (t * 4.0);
        int cc_q = (int)(cc * 4096.0 + 0.5);
        if (cc_q > 65535) cc_q = 65535;
        s_cc_q12[i] = (uint16_t)cc_q;
    }

    for (int m = 0; m < NUM_MATERIALS; m++) {
        double rough = (double)materials[m].roughness;
        double alpha = rough * rough;
        double a2 = alpha * alpha;
        double a2_m1 = a2 - 1.0;
        double k_smith = ((rough + 1.0) * (rough + 1.0)) * 0.125;
        double om_k = 1.0 - k_smith;
        for (int i = 0; i <= 1024; i++) {
            double t = (double)i * (1.0 / 1024.0);
            double d_denom = t * t * a2_m1 + 1.0;
            double dd = d_denom * d_denom + 1e-7;
            double d_base = (a2 * inv_pi * 0.055) / dd;
            if (t < 0.25) d_base *= (t * 4.0);
            uint32_t dq = (uint32_t)(d_base * 4096.0 + 0.5);
            if (dq > 524287u) dq = 524287u;
            s_ggx_q12[m][i] = dq;

            double g1 = t / (t * om_k + k_smith + 1e-6);
            int gq = (int)(g1 * 4096.0 + 0.5);
            if (gq > 4096) gq = 4096;
            s_smith_g_q12[m][i] = (uint16_t)gq;
        }
    }

    /* 7. 64x64 Interleaved Gradient Noise dither tile [-7 .. +7] */
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            float p = 0.06711056f * (float)x + 0.00583715f * (float)y;
            p -= (float)(int)p;
            float q = 52.9829189f * p;
            q -= (float)(int)q;
            int v = (int)(q * 15.0f) - 7;
            if (v < -7) v = -7; else if (v > 7) v = 7;
            s_ign_tile[y][x] = (int8_t)v;
        }
    }

    for (int i = 0; i <= 32768; i++) {
        s_q12_to_f64[i] = (double)i * (1.0 / 4096.0);
    }

    s_tables_ready = true;
}

static inline void write_q12_f64_channel(double *dst, int32_t v) {
    *dst = (v > 0) ? ((double)v * (1.0 / 4096.0)) : 0.0;
}

static inline void norm_vec3_q14(int32_t nx, int32_t ny, int32_t nz,
                                 int32_t *ox, int32_t *oy, int32_t *oz) {
    uint32_t len2 = (uint32_t)(nx * nx + ny * ny + nz * nz);
    if (len2 == 0u) {
        *ox = 0; *oy = 0; *oz = 0;
        return;
    }
    int e = 31 - __builtin_clz(len2);
    uint32_t inv;
    int shift;
    if (e >= 10) {
        int p = (e - 10) & ~1;
        uint32_t m = len2 >> p;
        if (m >= 4096u) m = 4095u;
        inv = (uint32_t)s_rsqrt_q20[m];
        shift = 6 + (p >> 1);
    } else {
        int p = (10 - e + 1) & ~1;
        uint32_t m = len2 << p;
        if (m >= 4096u) m = 4095u;
        inv = (uint32_t)s_rsqrt_q20[m];
        shift = 6 - (p >> 1);
    }
    int32_t rx = (nx * (int32_t)inv) >> shift;
    int32_t ry = (ny * (int32_t)inv) >> shift;
    int32_t rz = (nz * (int32_t)inv) >> shift;
    if (rx < -16384) rx = -16384; else if (rx > 16384) rx = 16384;
    if (ry < -16384) ry = -16384; else if (ry > 16384) ry = 16384;
    if (rz < -16384) rz = -16384; else if (rz > 16384) rz = 16384;
    *ox = rx; *oy = ry; *oz = rz;
}

static inline void sample_cubemap_q8(int rx, int ry, int rz, int out[3]) {
    int ax = (rx >= 0) ? rx : -rx;
    int ay = (ry >= 0) ? ry : -ry;
    int az = (rz >= 0) ? rz : -rz;

    int face, uc, vc, m;
    if (ax >= ay && ax >= az) {
        m = ax;
        if (rx >= 0) { face = 0; uc = -rz; vc = -ry; }
        else         { face = 1; uc =  rz; vc = -ry; }
    } else if (ay >= az) {
        m = ay;
        if (ry >= 0) { face = 2; uc =  rx; vc =  rz; }
        else         { face = 3; uc =  rx; vc = -rz; }
    } else {
        m = az;
        if (rz >= 0) { face = 4; uc =  rx; vc = -ry; }
        else         { face = 5; uc = -rx; vc = -ry; }
    }

    if (m < 1) m = 1;
    else if (m > 512) m = 512;
    int rcp = (int)s_rcp_q15[m];

    int u_q8 = (uc * rcp) >> 7;
    int v_q8 = (vc * rcp) >> 7;
    if (u_q8 < -256) u_q8 = -256; else if (u_q8 > 256) u_q8 = 256;
    if (v_q8 < -256) v_q8 = -256; else if (v_q8 > 256) v_q8 = 256;

    int tx = ((u_q8 + 256) * CUBE_UV_MAX) >> 9;
    int ty = ((v_q8 + 256) * CUBE_UV_MAX) >> 9;
    int x0 = tx >> 8;
    int y0 = ty >> 8;
    int fx = tx & 255;
    int fy = ty & 255;

    const uint16_t *p00 = s_env_cube[face][y0][x0];
    const uint16_t *p10 = s_env_cube[face][y0][x0 + 1];
    const uint16_t *p01 = s_env_cube[face][y0 + 1][x0];
    const uint16_t *p11 = s_env_cube[face][y0 + 1][x0 + 1];

    int ifx = 256 - fx;
    int ify = 256 - fy;
    int w00 = (ifx * ify) >> 8;
    int w10 = (fx  * ify) >> 8;
    int w01 = (ifx * fy)  >> 8;
    int w11 = (fx  * fy)  >> 8;

    out[0] = (p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11) >> 8;
    out[1] = (p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11) >> 8;
    out[2] = (p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11) >> 8;
}

static inline uint8_t rgb_dither_to_wm8(int r, int g, int b, int bias) {
    int rd = r + bias;
    int gd = g + bias;
    int bd = b + bias;
    if (rd < 0) rd = 0; else if (rd > 255) rd = 255;
    if (gd < 0) gd = 0; else if (gd > 255) gd = 255;
    if (bd < 0) bd = 0; else if (bd > 255) bd = 255;
    return s_rgb555_wm_lut[((rd >> 3) << 10) | ((gd >> 3) << 5) | (bd >> 3)];
}

/* Interleaved Gradient Noise tile initialized in init_tables() */

/* =========================================================================
 * Window Events & Input
 * ========================================================================= */
static void on_window_event(wm_window_t *w, uint32_t event, uint32_t data) {
    (void)w;
    if (event == WM_EVENT_CLOSE) {
        app_running = false;
    } else if (event == WM_EVENT_KEY_DOWN) {
        uint8_t sc = (uint8_t)(data & 0xFF);
        if (sc == SC_ESC || sc == SC_Q) {
            app_running = false;
        } else if (sc == SC_LEFT) {
            rot_y -= 6.0f;
            auto_spin = false;
        } else if (sc == SC_RIGHT) {
            rot_y += 6.0f;
            auto_spin = false;
        } else if (sc == SC_UP) {
            rot_x -= 6.0f;
            if (rot_x < -40.0f) rot_x = -40.0f;
            auto_spin = false;
        } else if (sc == SC_DOWN) {
            rot_x += 6.0f;
            if (rot_x > 75.0f) rot_x = 75.0f;
            auto_spin = false;
        } else if (sc == SC_SPACE) {
            auto_spin = !auto_spin;
        } else if (sc == SC_M) {
            current_mat = (current_mat + 1) % NUM_MATERIALS;
        } else if (sc == SC_S) {
            shadows_enabled = !shadows_enabled;
        } else if (sc == SC_L) {
            light1_enabled = !light1_enabled;
        } else if (sc == SC_R) {
            rot_x = 18.0f;
            rot_y = -28.0f;
            cam_dist = 5.4f;
            auto_spin = true;
            shadows_enabled = true;
            light1_enabled = true;
        } else if (sc == SC_PAGE_UP || sc == SC_KEYPAD_MINUS) {
            cam_dist -= 0.45f;
            if (cam_dist < 3.4f) cam_dist = 3.4f;
        } else if (sc == SC_PAGE_DOWN || sc == SC_KEYPAD_PLUS) {
            cam_dist += 0.45f;
            if (cam_dist > 14.0f) cam_dist = 14.0f;
        }
    }
}

/* =========================================================================
 * Real 3D Baked Solid-Angle Ambient Occlusion
 * ========================================================================= */
static uint8_t  *s_teapot_ao = NULL;
static float    *s_teapot_normals = NULL;
typedef struct {
    float   px, py, pz;
    float   nx, ny, nz;
    uint8_t ao;
} tp_subv_t;
static tp_subv_t *s_sub_verts = NULL;
static uint32_t  *s_sub_idx = NULL;
static uint32_t   s_sub_vcount = 0u;
static uint32_t   s_sub_tcount = 0u;
static bool s_mesh_inited = false;
typedef struct { float x, y, z, area; } ao_sample_t;

static void tp_eval_pn_midpoint(const float p0[3], const float wn0[3], const float sn0[3], uint8_t ao0,
                                const float p1[3], const float wn1[3], const float sn1[3], uint8_t ao1,
                                tp_subv_t *out) {
    bool swap = false;
    if (p1[0] < p0[0] - 1e-6f) swap = true;
    else if (fabsf(p1[0] - p0[0]) <= 1e-6f) {
        if (p1[1] < p0[1] - 1e-6f) swap = true;
        else if (fabsf(p1[1] - p0[1]) <= 1e-6f && p1[2] < p0[2] - 1e-6f) swap = true;
    }
    const float *pa = swap ? p1 : p0, *pb = swap ? p0 : p1;
    const float *na = swap ? wn1 : wn0, *nb = swap ? wn0 : wn1;

    float dx = pb[0] - pa[0], dy = pb[1] - pa[1], dz = pb[2] - pa[2];
    float dota = dx * na[0] + dy * na[1] + dz * na[2];
    float dotb = dx * nb[0] + dy * nb[1] + dz * nb[2];
    out->px = 0.5f * (pa[0] + pb[0]) + 0.125f * (dotb * nb[0] - dota * na[0]);
    out->py = 0.5f * (pa[1] + pb[1]) + 0.125f * (dotb * nb[1] - dota * na[1]);
    out->pz = 0.5f * (pa[2] + pb[2]) + 0.125f * (dotb * nb[2] - dota * na[2]);

    float nx = sn0[0] + sn1[0], ny = sn0[1] + sn1[1], nz = sn0[2] + sn1[2];
    float nl2 = nx * nx + ny * ny + nz * nz;
    if (nl2 > 1e-8f) {
        float inv = 1.0f / gl_sqrtf(nl2);
        nx *= inv; ny *= inv; nz *= inv;
    } else {
        nx = 0.5f * (na[0] + nb[0]);
        ny = 0.5f * (na[1] + nb[1]);
        nz = 0.5f * (na[2] + nb[2]);
    }
    out->nx = nx; out->ny = ny; out->nz = nz;
    out->ao = (uint8_t)(((uint32_t)ao0 + (uint32_t)ao1 + 1u) >> 1);
}

static void init_teapot_data(void) {
    init_tables();
    if (s_mesh_inited) return;

    s_teapot_ao = (uint8_t *)malloc((size_t)TEAPOT_NUM_VERTICES);
    s_teapot_normals = (float *)malloc((size_t)TEAPOT_NUM_VERTICES * 3u * sizeof(float));
    int max_ao_samples = (TEAPOT_NUM_TRIANGLES + 44) / 45;
    ao_sample_t *ao_samples = (ao_sample_t *)malloc((size_t)(max_ao_samples > 0 ? max_ao_samples : 1) * sizeof(ao_sample_t));
    if (!s_teapot_ao || !s_teapot_normals || !ao_samples) {
        free(ao_samples);
        free(s_teapot_ao); s_teapot_ao = NULL;
        free(s_teapot_normals); s_teapot_normals = NULL;
        return;
    }

    for (int i = 0; i < TEAPOT_NUM_VERTICES; i++) {
        const float *v = &teapot_vertices[i * 6];
        s_teapot_normals[i * 3 + 0] = v[3];
        s_teapot_normals[i * 3 + 1] = v[4];
        s_teapot_normals[i * 3 + 2] = v[5];
    }

    /* Weld Bezier patch boundary normals using O(N) spatial hash grid */
    {
        const uint32_t ph_sz = 16384u;
        int32_t *ph_head = (int32_t *)malloc((size_t)ph_sz * sizeof(int32_t));
        int32_t *ph_next = (int32_t *)malloc((size_t)TEAPOT_NUM_VERTICES * sizeof(int32_t));
        if (ph_head && ph_next) {
            memset(ph_head, 0xFF, (size_t)ph_sz * sizeof(int32_t));
            for (int i = 0; i < TEAPOT_NUM_VERTICES; i++) {
                const float *vi = &teapot_vertices[i * 6];
                int gx = (int)floorf(vi[0] * 100.0f);
                int gy = (int)floorf(vi[1] * 100.0f);
                int gz = (int)floorf(vi[2] * 100.0f);
                uint32_t h = ((uint32_t)(gx * 73856093) ^ (uint32_t)(gy * 19349663) ^ (uint32_t)(gz * 83492791)) & (ph_sz - 1u);
                ph_next[i] = ph_head[h];
                ph_head[h] = i;
            }
            for (int i = 0; i < TEAPOT_NUM_VERTICES; i++) {
                const float *vi = &teapot_vertices[i * 6];
                float sx = vi[3], sy = vi[4], sz = vi[5];
                int merges = 1;
                int gx0 = (int)floorf(vi[0] * 100.0f);
                int gy0 = (int)floorf(vi[1] * 100.0f);
                int gz0 = (int)floorf(vi[2] * 100.0f);
                for (int dz = -1; dz <= 1; dz++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            uint32_t h = ((uint32_t)((gx0 + dx) * 73856093) ^
                                          (uint32_t)((gy0 + dy) * 19349663) ^
                                          (uint32_t)((gz0 + dz) * 83492791)) & (ph_sz - 1u);
                            for (int j = ph_head[h]; j >= 0; j = ph_next[j]) {
                                if (j == i) continue;
                                const float *vj = &teapot_vertices[j * 6];
                                float ddx = vj[0] - vi[0], ddy = vj[1] - vi[1], ddz = vj[2] - vi[2];
                                if (ddx * ddx + ddy * ddy + ddz * ddz < 1e-5f) {
                                    float dot = vi[3] * vj[3] + vi[4] * vj[4] + vi[5] * vj[5];
                                    if (dot > 0.70f) {
                                        sx += vj[3]; sy += vj[4]; sz += vj[5];
                                        merges++;
                                    }
                                }
                            }
                        }
                    }
                }
                if (merges > 1) {
                    float l2 = sx * sx + sy * sy + sz * sz;
                    if (l2 > 1e-8f) {
                        float inv = 1.0f / gl_sqrtf(l2);
                        s_teapot_normals[i * 3 + 0] = sx * inv;
                        s_teapot_normals[i * 3 + 1] = sy * inv;
                        s_teapot_normals[i * 3 + 2] = sz * inv;
                    }
                }
            }
        }
        free(ph_head);
        free(ph_next);
    }

    /* Build representative surface emitter samples for fast true 3D AO */
    int n_samples = 0;
    for (int t = 0; t < TEAPOT_NUM_TRIANGLES && n_samples < max_ao_samples; t += 45) {
        const float *a = &teapot_vertices[teapot_indices[t * 3 + 0] * 6];
        const float *b = &teapot_vertices[teapot_indices[t * 3 + 1] * 6];
        const float *c = &teapot_vertices[teapot_indices[t * 3 + 2] * 6];
        float cx = (a[0] + b[0] + c[0]) * 0.333333f;
        float cy = (a[1] + b[1] + c[1]) * 0.333333f;
        float cz = (a[2] + b[2] + c[2]) * 0.333333f;
        float e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
        float e2x = c[0] - a[0], e2y = c[1] - a[1], e2z = c[2] - a[2];
        float crx = e1y * e2z - e1z * e2y;
        float cry = e1z * e2x - e1x * e2z;
        float crz = e1x * e2y - e1y * e2x;
        float area = 0.5f * gl_sqrtf(crx * crx + cry * cry + crz * crz);
        ao_samples[n_samples++] = (ao_sample_t){ cx, cy, cz, area * 39.0f };
    }

    for (int i = 0; i < TEAPOT_NUM_VERTICES; i++) {
        const float *v = &teapot_vertices[i * 6];
        float px = v[0], py = v[1], pz = v[2];
        float nx = s_teapot_normals[i * 3 + 0];
        float ny = s_teapot_normals[i * 3 + 1];
        float nz = s_teapot_normals[i * 3 + 2];

        /* Pedestal plane solid-angle occlusion at y = -0.910 */
        float occ = 0.0f;
        float dy_floor = py + 0.910f;
        if (dy_floor < 0.0f) dy_floor = 0.0f;
        float floor_occ = (0.42f * (1.0f - ny * 0.65f)) / (1.0f + dy_floor * 3.8f);
        if (floor_occ > 0.0f) occ += floor_occ;

        /* Self-occlusion from nearby forward-facing surface patches */
        float patch_occ = 0.0f;
        for (int s = 0; s < n_samples; s++) {
            float dx = ao_samples[s].x - px;
            float dy = ao_samples[s].y - py;
            float dz = ao_samples[s].z - pz;
            float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < 0.012f || d2 > 2.25f) continue;
            float ndotd = nx * dx + ny * dy + nz * dz;
            if (ndotd <= 0.0f) continue;
            float inv_d = 1.0f / gl_sqrtf(d2);
            float cos_t = ndotd * inv_d;
            patch_occ += (cos_t * ao_samples[s].area) / (d2 + 0.14f);
        }
        occ += patch_occ * 0.048f;

        /* Deepen lid overhang crease & knob collar cleanly */
        float r_xz = gl_sqrtf(px * px + pz * pz);
        if (py > 0.56f && py < 0.80f && r_xz < 1.08f && r_xz > 0.68f) {
            float d = (py - 0.68f) / 0.12f;
            occ += (1.0f - d * d) * 0.24f;
        }

        float ao = 1.0f - occ;
        if (ao < 0.22f) ao = 0.22f;
        if (ao > 1.0f) ao = 1.0f;
        s_teapot_ao[i] = (uint8_t)(ao * 255.0f + 0.5f);
    }

    free(ao_samples);

    /* Build shared 1-level watertight PN-subdivided mesh for both CPU and VirGL GPU */
    {
        float    *weld_n = (float *)malloc((size_t)TEAPOT_NUM_VERTICES * 3u * sizeof(float));
        const uint32_t ht_sz = 65536u;
        uint32_t *ht_key = (uint32_t *)malloc((size_t)ht_sz * sizeof(uint32_t));
        uint32_t *ht_val = (uint32_t *)malloc((size_t)ht_sz * sizeof(uint32_t));
        uint32_t  vcap   = (uint32_t)TEAPOT_NUM_VERTICES + (uint32_t)TEAPOT_NUM_TRIANGLES * 2u;
        tp_subv_t *sv    = (tp_subv_t *)malloc((size_t)vcap * sizeof(tp_subv_t));
        uint32_t  *si    = (uint32_t *)malloc((size_t)TEAPOT_NUM_TRIANGLES * 12u * sizeof(uint32_t));
        if (weld_n && ht_key && ht_val && sv && si) {
            memset(ht_key, 0xFF, (size_t)ht_sz * sizeof(uint32_t));
            const uint32_t wh_sz = 16384u;
            int32_t *wh_head = (int32_t *)malloc((size_t)wh_sz * sizeof(int32_t));
            int32_t *wh_next = (int32_t *)malloc((size_t)TEAPOT_NUM_VERTICES * sizeof(int32_t));
            if (wh_head && wh_next) {
                memset(wh_head, 0xFF, (size_t)wh_sz * sizeof(int32_t));
                for (int i = 0; i < TEAPOT_NUM_VERTICES; i++) {
                    const float *vi = &teapot_vertices[i * 6];
                    int gx = (int)floorf(vi[0] * 100.0f);
                    int gy = (int)floorf(vi[1] * 100.0f);
                    int gz = (int)floorf(vi[2] * 100.0f);
                    uint32_t h = ((uint32_t)(gx * 73856093) ^ (uint32_t)(gy * 19349663) ^ (uint32_t)(gz * 83492791)) & (wh_sz - 1u);
                    wh_next[i] = wh_head[h];
                    wh_head[h] = i;
                }
                for (int i = 0; i < TEAPOT_NUM_VERTICES; i++) {
                    const float *vi = &teapot_vertices[i * 6];
                    float sx = vi[3], sy = vi[4], sz = vi[5];
                    int gx0 = (int)floorf(vi[0] * 100.0f);
                    int gy0 = (int)floorf(vi[1] * 100.0f);
                    int gz0 = (int)floorf(vi[2] * 100.0f);
                    for (int dz = -1; dz <= 1; dz++) {
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) {
                                uint32_t h = ((uint32_t)((gx0 + dx) * 73856093) ^
                                              (uint32_t)((gy0 + dy) * 19349663) ^
                                              (uint32_t)((gz0 + dz) * 83492791)) & (wh_sz - 1u);
                                for (int j = wh_head[h]; j >= 0; j = wh_next[j]) {
                                    if (j == i) continue;
                                    const float *vj = &teapot_vertices[j * 6];
                                    float ddx = vj[0] - vi[0], ddy = vj[1] - vi[1], ddz = vj[2] - vi[2];
                                    if (ddx * ddx + ddy * ddy + ddz * ddz < 1e-5f) {
                                        sx += vj[3]; sy += vj[4]; sz += vj[5];
                                    }
                                }
                            }
                        }
                    }
                    float l2 = sx * sx + sy * sy + sz * sz;
                    if (l2 > 1e-8f) {
                        float inv = 1.0f / gl_sqrtf(l2);
                        weld_n[i * 3 + 0] = sx * inv;
                        weld_n[i * 3 + 1] = sy * inv;
                        weld_n[i * 3 + 2] = sz * inv;
                    } else {
                        weld_n[i * 3 + 0] = s_teapot_normals[i * 3 + 0];
                        weld_n[i * 3 + 1] = s_teapot_normals[i * 3 + 1];
                        weld_n[i * 3 + 2] = s_teapot_normals[i * 3 + 2];
                    }
                    sv[i] = (tp_subv_t){
                        vi[0], vi[1], vi[2],
                        s_teapot_normals[i * 3 + 0], s_teapot_normals[i * 3 + 1], s_teapot_normals[i * 3 + 2],
                        s_teapot_ao[i]
                    };
                }
            }
            free(wh_head);
            free(wh_next);
            uint32_t vc = (uint32_t)TEAPOT_NUM_VERTICES;
            for (uint32_t t = 0u; t < (uint32_t)TEAPOT_NUM_TRIANGLES; t++) {
                uint32_t tri_v[3] = {
                    teapot_indices[t * 3u + 0u],
                    teapot_indices[t * 3u + 1u],
                    teapot_indices[t * 3u + 2u]
                };
                uint32_t mid_v[3];
                for (int e = 0; e < 3; e++) {
                    uint32_t ia = tri_v[e], ib = tri_v[(e + 1) % 3];
                    uint32_t lo = (ia < ib) ? ia : ib;
                    uint32_t hi = (ia < ib) ? ib : ia;
                    uint32_t key = (lo << 16) | hi;
                    uint32_t slot = ((key * 2654435761u) >> 16) & (ht_sz - 1u);
                    uint32_t found = UINT32_MAX;
                    while (ht_key[slot] != UINT32_MAX) {
                        if (ht_key[slot] == key) { found = ht_val[slot]; break; }
                        slot = (slot + 1u) & (ht_sz - 1u);
                    }
                    if (found == UINT32_MAX) {
                        if (vc >= vcap) {
                            uint32_t nvcap = vcap * 2u;
                            tp_subv_t *nsv = (tp_subv_t *)realloc(sv, (size_t)nvcap * sizeof(tp_subv_t));
                            if (!nsv) { found = ia; goto pn_oom_edge; }
                            sv = nsv;
                            vcap = nvcap;
                        }
                        found = vc++;
                        tp_eval_pn_midpoint(&teapot_vertices[ia * 6u], &weld_n[ia * 3u], &s_teapot_normals[ia * 3u], s_teapot_ao[ia],
                                            &teapot_vertices[ib * 6u], &weld_n[ib * 3u], &s_teapot_normals[ib * 3u], s_teapot_ao[ib],
                                            &sv[found]);
                        ht_key[slot] = key;
                        ht_val[slot] = found;
                    }
                    pn_oom_edge:
                    mid_v[e] = found;
                }
                uint32_t ib = t * 12u;
                si[ib + 0u] = tri_v[0]; si[ib + 1u]  = mid_v[0]; si[ib + 2u]  = mid_v[2];
                si[ib + 3u] = mid_v[0]; si[ib + 4u]  = tri_v[1]; si[ib + 5u]  = mid_v[1];
                si[ib + 6u] = mid_v[2]; si[ib + 7u]  = mid_v[1]; si[ib + 8u]  = tri_v[2];
                si[ib + 9u] = mid_v[0]; si[ib + 10u] = mid_v[1]; si[ib + 11u] = mid_v[2];
            }
            s_sub_verts  = sv;
            s_sub_idx    = si;
            s_sub_vcount = vc;
            s_sub_tcount = (uint32_t)TEAPOT_NUM_TRIANGLES * 4u;
            sv = NULL; si = NULL;
        }
        free(weld_n); free(ht_key); free(ht_val); free(sv); free(si);
    }

    s_mesh_inited = true;
}

/* =========================================================================
 * Integer Fixed-Point G-Buffer & Two-Pass Soft Shadow Buffers
 * ========================================================================= */
typedef struct {
    int16_t nx, ny, nz;  /* High-precision eye-space normal [-16383..16383] */
    uint8_t ao;          /* Baked AO / pedestal r2_q [0..255] */
    uint8_t mat;         /* 0=bg, 1=teapot, 2=ped_top, 3=ped_side, 4=ped_bevel */
} gbuf_px_t;

typedef struct {
    int16_t nx, ny, nz;
    uint8_t ao;
    uint8_t valid;
} mirr_px_t;

typedef struct {
    float sx, sy, sz;
    int32_t nx_fp, ny_fp, nz_fp;
    int32_t ao_fp;
} gvert_t;

typedef struct {
    float sx, sy;
} svert_t;

static gvert_t   *s_tverts = NULL;
static gvert_t   *s_mverts = NULL;
static svert_t   *s_sverts = NULL;
static uint16_t  *s_zbuf = NULL;
static gbuf_px_t *s_gbuf = NULL;
static uint16_t  *s_ped_zbuf = NULL;
static gbuf_px_t *s_ped_gbuf = NULL;
static uint16_t  *s_mirr_zbuf = NULL;
static mirr_px_t *s_mirr_gbuf = NULL;
static uint8_t   *s_shd_mask = NULL;
static uint8_t   *s_shd_tmp = NULL;
static uint8_t   *s_shd_blur = NULL;
static size_t     s_buf_cap = 0;

/* Cached static studio backdrop + pre-shaded unshadowed pedestal */
static uint8_t       *s_bg_idx_cache = NULL;
static ipo_rgba64f_t *s_studio_bg_hdr = NULL;
static ipo_rgba64f_t *s_bg_hdr_cache = NULL;
static ipo_rgba64f_t *s_hdr_fb = NULL;
static size_t         s_hdr_cap = 0;
static int            s_bg_w = 0, s_bg_h = 0;

/* Track screen bounding box of geometry + shadow to avoid redundant full-screen work */
static int s_cur_bx0 = 0, s_cur_by0 = 0, s_cur_bx1 = -1, s_cur_by1 = -1;
static int s_prev_bx0 = 0, s_prev_by0 = 0, s_prev_bx1 = -1, s_prev_by1 = -1;
static int s_mirr_bx0 = 0, s_mirr_by0 = 0, s_mirr_bx1 = -1, s_mirr_by1 = -1;
static int s_mirr_prev_x0 = 0, s_mirr_prev_y0 = 0, s_mirr_prev_x1 = -1, s_mirr_prev_y1 = -1;
static int s_ped_bx0 = 0, s_ped_by0 = 0, s_ped_bx1 = -1, s_ped_by1 = -1;
static int s_shd_prev_x0 = 0, s_shd_prev_y0 = 0, s_shd_prev_x1 = -1, s_shd_prev_y1 = -1;
static int s_shd_full_prev_x0 = 0, s_shd_full_prev_y0 = 0, s_shd_full_prev_x1 = -1, s_shd_full_prev_y1 = -1;
static int s_shd_full_cur_x0 = 0, s_shd_full_cur_y0 = 0, s_shd_full_cur_x1 = -1, s_shd_full_cur_y1 = -1;
static int s_dirty_x0 = 0, s_dirty_y0 = 0, s_dirty_x1 = -1, s_dirty_y1 = -1;
static bool s_dirty_full_frame = true;
static int s_prev_vp_w = 0, s_prev_vp_h = 0;
static bool s_hdr_bg_primed = false;
static bool s_ped_cache_valid = false;
static float s_ped_cache_rotx = -9999.0f;
static float s_ped_cache_cam  = -9999.0f;
static int   s_ped_cache_mat  = -1;
static bool  s_ped_cache_shd  = false;
static bool  s_ped_cache_l1   = false;
static bool  s_ped_cache_hdr  = false;

static void shade_pedestal_pass_64f(int w, int sy0, int sy1, int sx0, int sx1,
                                    ipo_rgba64f_t *dst64, int mat_idx, bool has_soft_shadow);

/* Per-column view ray slopes (for fast eye-space view vector V(x,y)) */
typedef struct { int16_t x, y, z, pad; } vdir_q14_t;
static int16_t    *s_vx_col = NULL;
static int16_t    *s_vy_row = NULL;
static int16_t    *s_vx_col_q14 = NULL;
static int16_t    *s_vy_row_q14 = NULL;
static vdir_q14_t *s_vdir_q14 = NULL;
static int         s_ray_w = 0, s_ray_h = 0;

static void ensure_screen_buffers(int w, int h, int fb_h) {
    size_t need = (size_t)w * (size_t)h;
    int full_h = (fb_h > h) ? fb_h : h;
    size_t hdr_need = (size_t)w * (size_t)full_h;
    bool vp_changed = (s_prev_vp_w != w || s_prev_vp_h != h);
    if (need > s_buf_cap) {
        free(s_zbuf);
        free(s_gbuf);
        free(s_ped_zbuf);
        free(s_ped_gbuf);
        free(s_mirr_zbuf);
        free(s_mirr_gbuf);
        free(s_shd_mask);
        free(s_shd_tmp);
        free(s_shd_blur);
        s_zbuf      = (uint16_t *)malloc(need * sizeof(uint16_t));
        s_gbuf      = (gbuf_px_t *)malloc(need * sizeof(gbuf_px_t));
        s_ped_zbuf  = (uint16_t *)malloc(need * sizeof(uint16_t));
        s_ped_gbuf  = (gbuf_px_t *)malloc(need * sizeof(gbuf_px_t));
        s_mirr_zbuf = (uint16_t *)malloc(need * sizeof(uint16_t));
        s_mirr_gbuf = (mirr_px_t *)malloc(need * sizeof(mirr_px_t));
        s_shd_mask  = (uint8_t *)malloc(need);
        s_shd_tmp   = (uint8_t *)malloc(need);
        s_shd_blur  = (uint8_t *)malloc(need);
        if (!s_zbuf || !s_gbuf || !s_ped_zbuf || !s_ped_gbuf ||
            !s_mirr_zbuf || !s_mirr_gbuf ||
            !s_shd_mask || !s_shd_tmp || !s_shd_blur) {
            s_buf_cap = 0;
            return;
        }
        s_buf_cap = need;
        vp_changed = true;
    }
    if (vp_changed && s_zbuf && s_gbuf && s_ped_zbuf && s_ped_gbuf && s_mirr_zbuf && s_mirr_gbuf && s_shd_mask && s_shd_blur) {
        memset(s_zbuf, 0xFF, need * sizeof(uint16_t));
        memset(s_gbuf, 0, need * sizeof(gbuf_px_t));
        memset(s_ped_zbuf, 0xFF, need * sizeof(uint16_t));
        memset(s_ped_gbuf, 0, need * sizeof(gbuf_px_t));
        memset(s_mirr_zbuf, 0xFF, need * sizeof(uint16_t));
        memset(s_mirr_gbuf, 0, need * sizeof(mirr_px_t));
        memset(s_shd_mask, 0, need);
        memset(s_shd_blur, 0, need);
        s_prev_bx0 = 0; s_prev_by0 = 0; s_prev_bx1 = -1; s_prev_by1 = -1;
        s_cur_bx0 = 0;  s_cur_by0 = 0;  s_cur_bx1 = -1;  s_cur_by1 = -1;
        s_mirr_bx0 = 0; s_mirr_by0 = 0; s_mirr_bx1 = -1; s_mirr_by1 = -1;
        s_mirr_prev_x0 = 0; s_mirr_prev_y0 = 0; s_mirr_prev_x1 = -1; s_mirr_prev_y1 = -1;
        s_ped_bx0 = 0;  s_ped_by0 = 0;  s_ped_bx1 = -1;  s_ped_by1 = -1;
        s_shd_prev_x0 = 0; s_shd_prev_y0 = 0; s_shd_prev_x1 = -1; s_shd_prev_y1 = -1;
        s_shd_full_prev_x1 = -1; s_shd_full_prev_y1 = -1;
        s_shd_full_cur_x1 = -1;  s_shd_full_cur_y1 = -1;
        s_hdr_bg_primed = false;
        s_ped_cache_valid = false;
        s_prev_vp_w = w;
        s_prev_vp_h = h;
    }

    if (hdr_need > s_hdr_cap) {
        free(s_hdr_fb);
        s_hdr_fb = (ipo_rgba64f_t *)malloc(hdr_need * sizeof(ipo_rgba64f_t));
        s_hdr_cap = s_hdr_fb ? hdr_need : 0u;
        s_hdr_bg_primed = false;
    }

    if (s_ray_w != w || s_ray_h != h) {
        s_hdr_bg_primed = false;
        s_ped_cache_valid = false;
        free(s_vx_col);
        free(s_vy_row);
        free(s_vx_col_q14);
        free(s_vy_row_q14);
        free(s_vdir_q14);
        s_vdir_q14 = NULL;
        s_vx_col = (int16_t *)malloc((size_t)w * sizeof(int16_t));
        s_vy_row = (int16_t *)malloc((size_t)h * sizeof(int16_t));
        s_vx_col_q14 = (int16_t *)malloc((size_t)w * sizeof(int16_t));
        s_vy_row_q14 = (int16_t *)malloc((size_t)h * sizeof(int16_t));
        if (s_vx_col && s_vy_row && s_vx_col_q14 && s_vy_row_q14) {
            /* Match gluPerspective(42.0 deg) -> tan(21 deg) = 0.383864035 */
            double half_h = (double)h * 0.5;
            double half_w = (double)w * 0.5;
            double aspect = (h > 0) ? ((double)w / (double)h) : 1.333333333;
            double tan_half_y = 0.383864035035;
            double tan_half_x = tan_half_y * aspect;
            for (int x = 0; x < w; x++) {
                double u = (((double)x + 0.5) - half_w) / (half_w > 0.0 ? half_w : 1.0);
                double vx = -u * tan_half_x;
                s_vx_col_q14[x] = (int16_t)(vx * 16384.0 + (vx >= 0.0 ? 0.5 : -0.5));
                s_vx_col[x] = (int16_t)(vx * 256.0);
            }
            for (int y = 0; y < h; y++) {
                double v = (half_h - ((double)y + 0.5)) / (half_h > 0.0 ? half_h : 1.0);
                double vy = -v * tan_half_y;
                int32_t vy_q = (int32_t)(vy * 16384.0 + (vy >= 0.0 ? 0.5 : -0.5));
                s_vy_row_q14[y] = (int16_t)vy_q;
                s_vy_row[y] = (int16_t)(vy * 256.0);
            }
            s_vdir_q14 = (vdir_q14_t *)malloc((size_t)w * (size_t)h * sizeof(vdir_q14_t));
            if (s_vdir_q14) {
                for (int y = 0; y < h; y++) {
                    int32_t vy_ray = (int32_t)s_vy_row_q14[y];
                    vdir_q14_t *vrow = &s_vdir_q14[(size_t)y * (size_t)w];
                    for (int x = 0; x < w; x++) {
                        int32_t vx = 0, vy = 0, vz = 0;
                        norm_vec3_q14((int32_t)s_vx_col_q14[x], vy_ray, 15872, &vx, &vy, &vz);
                        vrow[x].x = (int16_t)vx;
                        vrow[x].y = (int16_t)vy;
                        vrow[x].z = (int16_t)vz;
                        vrow[x].pad = 0;
                    }
                }
            }
            s_ray_w = w;
            s_ray_h = h;
        }
    }
}

/* Smooth photographic studio backdrop: native linear RGBA64F + legacy 8bpp fallback */
static void ensure_backdrop_cache(int w, int h, bool want_hdr) {
    if (!want_hdr) ensure_wm8_lut();
    if (s_bg_w == w && s_bg_h == h &&
        (want_hdr ? (s_bg_hdr_cache != NULL && s_studio_bg_hdr != NULL) : (s_bg_idx_cache != NULL))) {
        return;
    }

    size_t count = (size_t)w * (size_t)h;
    if (want_hdr) {
        free(s_studio_bg_hdr);
        free(s_bg_hdr_cache);
        s_studio_bg_hdr = (ipo_rgba64f_t *)malloc(count * sizeof(ipo_rgba64f_t));
        s_bg_hdr_cache  = (ipo_rgba64f_t *)malloc(count * sizeof(ipo_rgba64f_t));
        if (!s_studio_bg_hdr || !s_bg_hdr_cache) {
            s_bg_w = 0; s_bg_h = 0;
            return;
        }
    } else {
        free(s_bg_idx_cache);
        s_bg_idx_cache = (uint8_t *)malloc(count);
        if (!s_bg_idx_cache) {
            s_bg_w = 0; s_bg_h = 0;
            return;
        }
    }
    s_hdr_bg_primed = false;
    s_ped_cache_valid = false;
    s_bg_w = w;
    s_bg_h = h;

    double cx = (double)w * 0.5;
    double cy = (double)h * 0.43;
    double inv_r2 = 1.0 / ((double)w * (double)w * 0.34 + (double)h * (double)h * 0.34);

    for (int y = 0; y < h; y++) {
        double dy = (double)y - cy;
        double dy2 = dy * dy;
        double grad_y = 1.0 - (double)y / (double)(h > 1 ? h : 1);
        for (int x = 0; x < w; x++) {
            double dx = (double)x - cx;
            double d2 = (dx * dx + dy2) * inv_r2;
            double halo = 1.0 - d2;
            if (halo < 0.0) halo = 0.0;
            halo = halo * halo * (1.25 - 0.25 * halo);

            double lum = 0.032 + halo * 0.145 + grad_y * 0.022;
            if (want_hdr) {
                ipo_rgba64f_t px = { lum * 0.96, lum * 0.99, lum * 1.05, 1.0 };
                size_t idx = (size_t)y * (size_t)w + (size_t)x;
                s_studio_bg_hdr[idx] = px;
                s_bg_hdr_cache[idx] = px;
            } else {
                const int8_t *irow = s_ign_tile[y & 63];
                int r8 = (int)(gl_sqrtf((float)lum * 0.96f) * 255.0f + 0.5f);
                int g8 = (int)(gl_sqrtf((float)lum * 0.99f) * 255.0f + 0.5f);
                int b8 = (int)(gl_sqrtf((float)lum * 1.05f) * 255.0f + 0.5f);
                int bias = ((int)irow[x & 63] * 5) >> 3;
                s_bg_idx_cache[(size_t)y * (size_t)w + (size_t)x] = rgb_dither_to_wm8(r8, g8, b8, bias);
            }
        }
    }
}

static inline void project_eye_to_screen(float ex, float ey, float ez,
                                         const GLfloat pj[16],
                                         float half_w, float half_h,
                                         float *sx, float *sy, float *sz) {
    float cx = pj[0] * ex + pj[8]  * ez;
    float cy = pj[5] * ey + pj[9]  * ez;
    float cz = pj[10] * ez + pj[14];
    float cw = -ez;
    float inv_w = (cw > 0.01f || cw < -0.01f) ? (1.0f / cw) : 1.0f;
    *sx = half_w + (cx * inv_w) * half_w;
    *sy = half_h - (cy * inv_w) * half_h;
    *sz = (cz * inv_w) * 0.5f + 0.5f;
}

static void raster_gbuf_tri_clipped(int w, int h,
                                    int clip_y0, int clip_y1,
                                    const gvert_t *v0,
                                    const gvert_t *v1,
                                    const gvert_t *v2,
                                    uint8_t mat_id,
                                    int *bx0_out, int *by0_out,
                                    int *bx1_out, int *by1_out) {
    const gvert_t *a = v0, *b = v1, *c = v2;
    if (a->sy > b->sy) { const gvert_t *t = a; a = b; b = t; }
    if (b->sy > c->sy) { const gvert_t *t = b; b = c; c = t; }
    if (a->sy > b->sy) { const gvert_t *t = a; a = b; b = t; }

    float total_h = c->sy - a->sy;
    if (total_h < 0.35f) return;

    int min_y = (int)(a->sy + 0.5f);
    int max_y = (int)(c->sy - 0.5f);
    if (min_y < clip_y0) min_y = clip_y0;
    if (max_y > clip_y1) max_y = clip_y1;
    if (min_y < 0) min_y = 0;
    if (max_y >= h) max_y = h - 1;
    if (min_y > max_y) return;

    float area = (b->sx - a->sx) * (c->sy - a->sy) -
                 (b->sy - a->sy) * (c->sx - a->sx);
    float abs_area = (area >= 0.0f) ? area : -area;
    if (abs_area < 0.05f) return;

    if (by0_out && min_y < *by0_out) *by0_out = min_y;
    if (by1_out && max_y > *by1_out) *by1_out = max_y;

    /* Per-triangle constant partial derivatives wrt screen x & y (Q14 for z, Q10 for n, Q16 for ao) */
    float inv_area = 1.0f / area;
    float dy_ca = c->sy - a->sy;
    float dy_ba = b->sy - a->sy;
    float dx_ca = c->sx - a->sx;
    float dx_ba = b->sx - a->sx;

    float za = a->sz * 65535.0f;
    float zb = b->sz * 65535.0f;
    float zc = c->sz * 65535.0f;
    if (za < 0.0f) za = 0.0f; else if (za > 65535.0f) za = 65535.0f;
    if (zb < 0.0f) zb = 0.0f; else if (zb > 65535.0f) zb = 65535.0f;
    if (zc < 0.0f) zc = 0.0f; else if (zc > 65535.0f) zc = 65535.0f;

    float dz_dx_f  = ((zb - za) * dy_ca - (zc - za) * dy_ba) * inv_area;
    float dz_dy_f  = (dx_ba * (zc - za) - dx_ca * (zb - za)) * inv_area;

    float nxa = (float)a->nx_fp, nxb = (float)b->nx_fp, nxc = (float)c->nx_fp;
    float nya = (float)a->ny_fp, nyb = (float)b->ny_fp, nyc = (float)c->ny_fp;
    float nza = (float)a->nz_fp, nzb = (float)b->nz_fp, nzc = (float)c->nz_fp;
    float aoa = (float)a->ao_fp, aob = (float)b->ao_fp, aoc = (float)c->ao_fp;

    int32_t dz_dx, dnx_dx, dny_dx, dnz_dx, dao_dx;
    int32_t dz_dy, dnx_dy, dny_dy, dnz_dy, dao_dy;
    if (abs_area >= 1.0f) {
        dz_dx  = (int32_t)(dz_dx_f * 16384.0f);
        dz_dy  = (int32_t)(dz_dy_f * 16384.0f);
        dnx_dx = (int32_t)(((nxb - nxa) * dy_ca - (nxc - nxa) * dy_ba) * inv_area);
        dnx_dy = (int32_t)((dx_ba * (nxc - nxa) - dx_ca * (nxb - nxa)) * inv_area);
        dny_dx = (int32_t)(((nyb - nya) * dy_ca - (nyc - nya) * dy_ba) * inv_area);
        dny_dy = (int32_t)((dx_ba * (nyc - nya) - dx_ca * (nyb - nya)) * inv_area);
        dnz_dx = (int32_t)(((nzb - nza) * dy_ca - (nzc - nza) * dy_ba) * inv_area);
        dnz_dy = (int32_t)((dx_ba * (nzc - nza) - dx_ca * (nzb - nza)) * inv_area);
        dao_dx = (int32_t)(((aob - aoa) * dy_ca - (aoc - aoa) * dy_ba) * inv_area);
        dao_dy = (int32_t)((dx_ba * (aoc - aoa) - dx_ca * (aob - aoa)) * inv_area);
    } else if (abs_area >= 0.45f) {
        dz_dx  = (int32_t)(dz_dx_f * 16384.0f);
        dz_dy  = (int32_t)(dz_dy_f * 16384.0f);
        dnx_dx = dnx_dy = dny_dx = dny_dy = dnz_dx = dnz_dy = dao_dx = dao_dy = 0;
    } else {
        dz_dx = dz_dy = dnx_dx = dnx_dy = dny_dx = dny_dy = dnz_dx = dnz_dy = dao_dx = dao_dy = 0;
    }

    int32_t z0_fp  = (int32_t)(za * 16384.0f);
    int32_t nx0_fp = a->nx_fp;
    int32_t ny0_fp = a->ny_fp;
    int32_t nz0_fp = a->nz_fp;
    int32_t ao0_fp = a->ao_fp;

    float inv_tot = 1.0f / total_h;
    float h_ab = b->sy - a->sy;
    float inv_ab = (h_ab > 1e-4f) ? (1.0f / h_ab) : 0.0f;
    float h_bc = c->sy - b->sy;
    float inv_bc = (h_bc > 1e-4f) ? (1.0f / h_bc) : 0.0f;

    float dx_ac_dy_f = (c->sx - a->sx) * inv_tot;
    float dx_ab_dy_f = (b->sx - a->sx) * inv_ab;
    float dx_bc_dy_f = (c->sx - b->sx) * inv_bc;

    float fy_start = (float)min_y + 0.5f;
    int32_t dac = (int32_t)(dx_ac_dy_f * 65536.0f);
    int32_t dab = (int32_t)(dx_ab_dy_f * 65536.0f);
    int32_t dbc = (int32_t)(dx_bc_dy_f * 65536.0f);

    int32_t x_ac = (int32_t)((a->sx + (fy_start - a->sy) * dx_ac_dy_f) * 65536.0f);
    int split_y = (int)(b->sy - 0.5f);
    int32_t x_ab = (int32_t)((a->sx + (fy_start - a->sy) * dx_ab_dy_f) * 65536.0f);
    int y_bc0 = (split_y + 1 > min_y) ? (split_y + 1) : min_y;
    int32_t x_bc = (int32_t)((b->sx + ((float)y_bc0 + 0.5f - b->sy) * dx_bc_dy_f) * 65536.0f);

    /* Anchor near triangle min_x so 32-bit interpolation deltas never overflow */
    float tri_min_xf = a->sx < b->sx ? a->sx : b->sx;
    if (c->sx < tri_min_xf) tri_min_xf = c->sx;
    int tri_min_x = (int)tri_min_xf;
    if (tri_min_x < 0) tri_min_x = 0; else if (tri_min_x >= w) tri_min_x = w - 1;

    float px_anchor = ((float)tri_min_x + 0.5f) - a->sx;
    float py_anchor = fy_start - a->sy;
    int32_t row_z  = z0_fp  + (int32_t)(px_anchor * (float)dz_dx  + py_anchor * (float)dz_dy);
    int32_t row_nx = nx0_fp + (int32_t)(px_anchor * (float)dnx_dx + py_anchor * (float)dnx_dy);
    int32_t row_ny = ny0_fp + (int32_t)(px_anchor * (float)dny_dx + py_anchor * (float)dny_dy);
    int32_t row_nz = nz0_fp + (int32_t)(px_anchor * (float)dnz_dx + py_anchor * (float)dnz_dy);
    int32_t row_ao = ao0_fp + (int32_t)(px_anchor * (float)dao_dx + py_anchor * (float)dao_dy);

    for (int y = min_y; y <= max_y; y++) {
        int32_t x1 = x_ac;
        int32_t x2 = (y <= split_y) ? x_ab : x_bc;
        if (x1 > x2) { int32_t t = x1; x1 = x2; x2 = t; }

        int min_x = (x1 + 32768) >> 16;
        int max_x = (x2 - 32768) >> 16;
        if (min_x < 0) min_x = 0;
        if (max_x >= w) max_x = w - 1;

        if (min_x <= max_x) {
            if (bx0_out && min_x < *bx0_out) *bx0_out = min_x;
            if (bx1_out && max_x > *bx1_out) *bx1_out = max_x;

            int32_t dx_span = (int32_t)(min_x - tri_min_x);
            int32_t cur_z  = row_z  + dx_span * dz_dx;
            int32_t cur_nx = row_nx + dx_span * dnx_dx;
            int32_t cur_ny = row_ny + dx_span * dny_dx;
            int32_t cur_nz = row_nz + dx_span * dnz_dx;
            int32_t cur_ao = row_ao + dx_span * dao_dx;

            size_t roff = (size_t)y * (size_t)w + (size_t)min_x;
            uint16_t  *zrow = &s_zbuf[roff];
            gbuf_px_t *grow = &s_gbuf[roff];
            int len = max_x - min_x + 1;

            for (int px = 0; px < len; px++) {
                uint16_t zv = (cur_z <= 0) ? 0u : (uint16_t)((uint32_t)cur_z >> 14);
                if (zv < zrow[px]) {
                    zrow[px] = zv;
                    int nx = cur_nx >> 10;
                    int ny = cur_ny >> 10;
                    int nz = cur_nz >> 10;
                    int ao = cur_ao >> 16;
                    if (nx < -16383) nx = -16383; else if (nx > 16383) nx = 16383;
                    if (ny < -16383) ny = -16383; else if (ny > 16383) ny = 16383;
                    if (nz < -16383) nz = -16383; else if (nz > 16383) nz = 16383;
                    if (ao < 0) ao = 0; else if (ao > 255) ao = 255;

                    grow[px].nx  = (int16_t)nx;
                    grow[px].ny  = (int16_t)ny;
                    grow[px].nz  = (int16_t)nz;
                    grow[px].ao  = (uint8_t)ao;
                    grow[px].mat = mat_id;
                }
                cur_z  += dz_dx;
                cur_nx += dnx_dx;
                cur_ny += dny_dx;
                cur_nz += dnz_dx;
                cur_ao += dao_dx;
            }
        }
        x_ac += dac;
        if (y <= split_y) x_ab += dab;
        else              x_bc += dbc;
        row_z  += dz_dy;
        row_nx += dnx_dy;
        row_ny += dny_dy;
        row_nz += dnz_dy;
        row_ao += dao_dy;
    }
}

static void raster_gbuf_tri(int w, int h,
                            const gvert_t *v0,
                            const gvert_t *v1,
                            const gvert_t *v2,
                            uint8_t mat_id) {
    raster_gbuf_tri_clipped(w, h, 0, h - 1, v0, v1, v2, mat_id,
                            &s_cur_bx0, &s_cur_by0, &s_cur_bx1, &s_cur_by1);
}

static void raster_shadow_mask_tri_clipped(int w, int h,
                                           int clip_y0, int clip_y1,
                                           const svert_t *v0,
                                           const svert_t *v1,
                                           const svert_t *v2) {
    float area = (v1->sx - v0->sx) * (v2->sy - v0->sy) -
                 (v1->sy - v0->sy) * (v2->sx - v0->sx);
    if (area >= 0.0f) return;

    const svert_t *a = v0, *b = v1, *c = v2;
    if (a->sy > b->sy) { const svert_t *t = a; a = b; b = t; }
    if (b->sy > c->sy) { const svert_t *t = b; b = c; c = t; }
    if (a->sy > b->sy) { const svert_t *t = a; a = b; b = t; }

    float total_h = c->sy - a->sy;
    if (total_h < 0.3f) return;

    float min_sx = v0->sx < v1->sx ? (v0->sx < v2->sx ? v0->sx : v2->sx) : (v1->sx < v2->sx ? v1->sx : v2->sx);
    float max_sx = v0->sx > v1->sx ? (v0->sx > v2->sx ? v0->sx : v2->sx) : (v1->sx > v2->sx ? v1->sx : v2->sx);
    if (max_sx < 0.0f || min_sx >= (float)w) return;

    int min_y = (int)(a->sy + 0.5f);
    int max_y = (int)(c->sy - 0.5f);
    if (min_y < clip_y0) min_y = clip_y0;
    if (max_y > clip_y1) max_y = clip_y1;
    if (min_y < 0) min_y = 0;
    if (max_y >= h) max_y = h - 1;
    if (min_y > max_y) return;

    float inv_tot = 1.0f / total_h;
    float h_ab = b->sy - a->sy;
    float inv_ab = (h_ab > 1e-4f) ? (1.0f / h_ab) : 0.0f;
    float h_bc = c->sy - b->sy;
    float inv_bc = (h_bc > 1e-4f) ? (1.0f / h_bc) : 0.0f;

    int32_t dac = (int32_t)(((c->sx - a->sx) * inv_tot) * 65536.0f);
    int32_t dab = (int32_t)(((b->sx - a->sx) * inv_ab) * 65536.0f);
    int32_t dbc = (int32_t)(((c->sx - b->sx) * inv_bc) * 65536.0f);

    float fy0 = (float)min_y + 0.5f;
    int32_t x_ac = (int32_t)((a->sx + (fy0 - a->sy) * ((c->sx - a->sx) * inv_tot)) * 65536.0f);
    int split_y = (int)(b->sy - 0.5f);
    int32_t x_ab = (int32_t)((a->sx + (fy0 - a->sy) * ((b->sx - a->sx) * inv_ab)) * 65536.0f);
    int32_t x_bc = (int32_t)((b->sx + ((float)((split_y + 1 > min_y ? split_y + 1 : min_y)) + 0.5f - b->sy) *
                             ((c->sx - b->sx) * inv_bc)) * 65536.0f);

    for (int y = min_y; y <= max_y; y++) {
        int32_t x1 = x_ac;
        int32_t x2 = (y <= split_y) ? x_ab : x_bc;
        if (x1 > x2) { int32_t t = x1; x1 = x2; x2 = t; }

        int min_x = (x1 + 32768) >> 16;
        int max_x = (x2 - 32768) >> 16;
        if (min_x < 0) min_x = 0;
        if (max_x >= w) max_x = w - 1;
        if (min_x <= max_x) {
            memset(s_shd_mask + (size_t)y * (size_t)w + (size_t)min_x,
                   255, (size_t)(max_x - min_x + 1));
        }

        x_ac += dac;
        if (y <= split_y) x_ab += dab;
        else              x_bc += dbc;
    }
}

static void build_shadow_matrix(float mat[16], const float light[4], const float plane[4]) {
    float dot = plane[0] * light[0] + plane[1] * light[1] + plane[2] * light[2] + plane[3] * light[3];
    mat[0]  = dot - light[0] * plane[0]; mat[4]  = -light[0] * plane[1];
    mat[8]  = -light[0] * plane[2];      mat[12] = -light[0] * plane[3];
    mat[1]  = -light[1] * plane[0];      mat[5]  = dot - light[1] * plane[1];
    mat[9]  = -light[1] * plane[2];      mat[13] = -light[1] * plane[3];
    mat[2]  = -light[2] * plane[0];      mat[6]  = -light[2] * plane[1];
    mat[10] = dot - light[2] * plane[2]; mat[14] = -light[2] * plane[3];
    mat[3]  = -light[3] * plane[0];      mat[7]  = -light[3] * plane[1];
    mat[11] = -light[3] * plane[2];      mat[15] = dot - light[3] * plane[3];
}

/* =========================================================================
 * Pass 1: Geometry -> Fixed-Point G-Buffer + Soft 5x5 Gaussian Shadow Pass
 * ========================================================================= */
#define PED_SEGS 192

static gvert_t s_ped_mid[PED_SEGS + 1];
static gvert_t s_ped_top[PED_SEGS + 1];
static gvert_t s_ped_bev[PED_SEGS + 1];
static gvert_t s_ped_bot[PED_SEGS + 1];

typedef struct {
    uint16_t tri_idx;
    int16_t  min_y;
    int16_t  max_y;
} vis_tri_t;

static vis_tri_t *s_vis_tris = NULL;
static vis_tri_t *s_vis_shd_tris = NULL;

typedef struct {
    float half_w, half_h, z_bias, z_scale;
    float m0, m4, m8, m12;
    float m1, m5, m9, m13;
    float m2, m6, m10, m14;
    float nm0, nm4, nm8;
    float nm1, nm5, nm9;
    float nm2, nm6, nm10;
    int   sub_vcount;
    bool  do_shadow;
    bool  do_mirror;
    float half_sw, half_sh;
    float sm0, sm4, sm8, sm12;
    float sm1, sm5, sm9, sm13;
    float sm3, sm7, sm11, sm15;
    volatile int next_v;
} tvert_job_t;

static void tvert_worker(int tid, void *ctx) {
    (void)tid;
    tvert_job_t *j = (tvert_job_t *)ctx;
    const float half_w = j->half_w, half_h = j->half_h;
    const float z_bias = j->z_bias, z_scale = j->z_scale;
    const float m0 = j->m0, m4 = j->m4, m8  = j->m8,  m12 = j->m12;
    const float m1 = j->m1, m5 = j->m5, m9  = j->m9,  m13 = j->m13;
    const float m2 = j->m2, m6 = j->m6, m10 = j->m10, m14 = j->m14;
    const float nm0 = j->nm0, nm4 = j->nm4, nm8  = j->nm8;
    const float nm1 = j->nm1, nm5 = j->nm5, nm9  = j->nm9;
    const float nm2 = j->nm2, nm6 = j->nm6, nm10 = j->nm10;
    const int   vcount = j->sub_vcount;
    const bool  do_sh  = j->do_shadow;
    const bool  do_mr  = j->do_mirror;
    const float half_sw = j->half_sw, half_sh = j->half_sh;
    const float sm0 = j->sm0, sm4 = j->sm4, sm8  = j->sm8,  sm12 = j->sm12;
    const float sm1 = j->sm1, sm5 = j->sm5, sm9  = j->sm9,  sm13 = j->sm13;
    const float sm3 = j->sm3, sm7 = j->sm7, sm11 = j->sm11, sm15 = j->sm15;

    for (;;) {
        int start = __atomic_fetch_add(&j->next_v, 256, __ATOMIC_RELAXED);
        if (start >= vcount) break;
        int end = start + 256;
        if (end > vcount) end = vcount;
        for (int i = start; i < end; i++) {
            float ox, oy, oz, onx, ony, onz;
            uint8_t ao_u;
            if (s_sub_verts) {
                const tp_subv_t *sv = &s_sub_verts[i];
                ox = sv->px; oy = sv->py; oz = sv->pz;
                onx = sv->nx; ony = sv->ny; onz = sv->nz;
                ao_u = sv->ao;
            } else {
                const float *v = &teapot_vertices[i * 6];
                const float *wn = &s_teapot_normals[i * 3];
                ox = v[0]; oy = v[1]; oz = v[2];
                onx = wn[0]; ony = wn[1]; onz = wn[2];
                ao_u = s_teapot_ao[i];
            }

            float ez = m2 * ox + m6 * oy + m10 * oz + m14;
            float inv_w = (ez < -0.01f || ez > 0.01f) ? (-1.0f / ez) : 1.0f;
            s_tverts[i].sx = half_w + (m0 * ox + m4 * oy + m8 * oz + m12) * inv_w;
            s_tverts[i].sy = half_h - (m1 * ox + m5 * oy + m9 * oz + m13) * inv_w;
            s_tverts[i].sz = z_bias + z_scale * inv_w;

            s_tverts[i].nx_fp = (int32_t)(nm0 * onx + nm4 * ony + nm8  * onz);
            s_tverts[i].ny_fp = (int32_t)(nm1 * onx + nm5 * ony + nm9  * onz);
            s_tverts[i].nz_fp = (int32_t)(nm2 * onx + nm6 * ony + nm10 * onz);
            s_tverts[i].ao_fp = (int32_t)ao_u << 16;

            if (i < TEAPOT_NUM_VERTICES) {
                const float *bv = &teapot_vertices[i * 6];
                const float *bwn = &s_teapot_normals[i * 3];
                float bx = bv[0], by = bv[1], bz = bv[2];
                if (do_sh) {
                    float cx = sm0 * bx + sm4 * by + sm8  * bz + sm12;
                    float cy = sm1 * bx + sm5 * by + sm9  * bz + sm13;
                    float cw = sm3 * bx + sm7 * by + sm11 * bz + sm15;
                    float inv_cw = (cw > 0.001f || cw < -0.001f) ? (1.0f / cw) : 1.0f;
                    s_sverts[i].sx = half_sw + cx * inv_cw;
                    s_sverts[i].sy = half_sh - cy * inv_cw;
                }
                if (do_mr) {
                    float my = -1.824f - by;
                    float mny = -bwn[1];
                    float mez = m2 * bx + m6 * my + m10 * bz + m14;
                    float minv_w = (mez < -0.01f || mez > 0.01f) ? (-1.0f / mez) : 1.0f;
                    s_mverts[i].sx = half_w + (m0 * bx + m4 * my + m8 * bz + m12) * minv_w;
                    s_mverts[i].sy = half_h - (m1 * bx + m5 * my + m9 * bz + m13) * minv_w;
                    s_mverts[i].sz = z_bias + z_scale * minv_w;
                    s_mverts[i].nx_fp = (int32_t)(nm0 * bwn[0] + nm4 * mny + nm8  * bwn[2]);
                    s_mverts[i].ny_fp = (int32_t)(nm1 * bwn[0] + nm5 * mny + nm9  * bwn[2]);
                    s_mverts[i].nz_fp = (int32_t)(nm2 * bwn[0] + nm6 * mny + nm10 * bwn[2]);
                    s_mverts[i].ao_fp = (int32_t)s_teapot_ao[i] << 16;
                }
            }
        }
    }
}

typedef struct {
    int vp_w, cx0, cy0, cy1;
    size_t span;
    volatile int next_y;
} ped_restore_job_t;

static void ped_restore_worker(int tid, void *ctx) {
    (void)tid;
    ped_restore_job_t *j = (ped_restore_job_t *)ctx;
    const int vp_w = j->vp_w, cx0 = j->cx0, cy1 = j->cy1;
    const size_t span = j->span;
    for (;;) {
        int y0 = __atomic_fetch_add(&j->next_y, 8, __ATOMIC_RELAXED);
        if (y0 > cy1) break;
        int y1 = y0 + 7;
        if (y1 > cy1) y1 = cy1;
        for (int y = y0; y <= y1; y++) {
            size_t off = (size_t)y * (size_t)vp_w + (size_t)cx0;
            memcpy(s_zbuf + off, s_ped_zbuf + off, span * sizeof(uint16_t));
            memcpy(s_gbuf + off, s_ped_gbuf + off, span * sizeof(gbuf_px_t));
        }
    }
}

typedef struct {
    int w, h;
    int vis_count;
    int *band_y0;
    int *band_y1;
    int band_count;
    volatile int next_band;
} raster_band_job_t;

static void shd_band_worker(int tid, void *ctx) {
    (void)tid;
    raster_band_job_t *j = (raster_band_job_t *)ctx;
    if (!j || !j->band_y0 || !j->band_y1) return;
    const int w = j->w, h = j->h, vc = j->vis_count;
    for (;;) {
        int b = __atomic_fetch_add(&j->next_band, 1, __ATOMIC_RELAXED);
        if (b >= j->band_count) break;
        int by0 = j->band_y0[b];
        int by1 = j->band_y1[b];
        for (int i = 0; i < vc; i++) {
            const vis_tri_t *vt = &s_vis_shd_tris[i];
            if ((int)vt->max_y < by0 || (int)vt->min_y > by1) continue;
            int idx = (int)vt->tri_idx * 3;
            raster_shadow_mask_tri_clipped(w, h, by0, by1,
                                           &s_sverts[teapot_indices[idx + 0]],
                                           &s_sverts[teapot_indices[idx + 1]],
                                           &s_sverts[teapot_indices[idx + 2]]);
        }
    }
}

static void raster_band_worker(int tid, void *ctx) {
    (void)tid;
    raster_band_job_t *j = (raster_band_job_t *)ctx;
    if (!j || !j->band_y0 || !j->band_y1) return;
    const int w = j->w, h = j->h, vc = j->vis_count;
    for (;;) {
        int b = __atomic_fetch_add(&j->next_band, 1, __ATOMIC_RELAXED);
        if (b >= j->band_count) break;
        int by0 = j->band_y0[b];
        int by1 = j->band_y1[b];
        for (int i = 0; i < vc; i++) {
            const vis_tri_t *vt = &s_vis_tris[i];
            if ((int)vt->max_y < by0 || (int)vt->min_y > by1) continue;
            uint32_t idx = (uint32_t)vt->tri_idx * 3u;
            uint32_t i0 = s_sub_idx ? s_sub_idx[idx + 0u] : teapot_indices[idx + 0u];
            uint32_t i1 = s_sub_idx ? s_sub_idx[idx + 1u] : teapot_indices[idx + 1u];
            uint32_t i2 = s_sub_idx ? s_sub_idx[idx + 2u] : teapot_indices[idx + 2u];
            raster_gbuf_tri_clipped(w, h, by0, by1,
                                    &s_tverts[i0], &s_tverts[i1], &s_tverts[i2],
                                    1, NULL, NULL, NULL, NULL);
        }
    }
}

static void raster_mirror_tri(int w, int clip_x0, int clip_y0, int clip_x1, int clip_y1,
                              const gvert_t *v0, const gvert_t *v1, const gvert_t *v2,
                              int *io_bx0, int *io_by0, int *io_bx1, int *io_by1) {
    float sx0 = v0->sx, sy0 = v0->sy;
    float sx1 = v1->sx, sy1 = v1->sy;
    float sx2 = v2->sx, sy2 = v2->sy;
    float area = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
    if (area <= 0.05f) return;

    float inv_a = 1.0f / area;
    float fmin_x = sx0 < sx1 ? (sx0 < sx2 ? sx0 : sx2) : (sx1 < sx2 ? sx1 : sx2);
    float fmax_x = sx0 > sx1 ? (sx0 > sx2 ? sx0 : sx2) : (sx1 > sx2 ? sx1 : sx2);
    float fmin_y = sy0 < sy1 ? (sy0 < sy2 ? sy0 : sy2) : (sy1 < sy2 ? sy1 : sy2);
    float fmax_y = sy0 > sy1 ? (sy0 > sy2 ? sy0 : sy2) : (sy1 > sy2 ? sy1 : sy2);
    int min_x = (int)fmin_x;
    int max_x = (int)fmax_x + 1;
    int min_y = (int)fmin_y;
    int max_y = (int)fmax_y + 1;
    if (min_x < clip_x0) min_x = clip_x0;
    if (max_x > clip_x1) max_x = clip_x1;
    if (min_y < clip_y0) min_y = clip_y0;
    if (max_y > clip_y1) max_y = clip_y1;
    if (min_x > max_x || min_y > max_y) return;

    float z0 = v0->sz * 65535.0f, z1 = v1->sz * 65535.0f, z2 = v2->sz * 65535.0f;
    float nx0 = (float)(v0->nx_fp >> 10), nx1 = (float)(v1->nx_fp >> 10), nx2 = (float)(v2->nx_fp >> 10);
    float ny0 = (float)(v0->ny_fp >> 10), ny1 = (float)(v1->ny_fp >> 10), ny2 = (float)(v2->ny_fp >> 10);
    float nz0 = (float)(v0->nz_fp >> 10), nz1 = (float)(v1->nz_fp >> 10), nz2 = (float)(v2->nz_fp >> 10);
    float ao0 = (float)(v0->ao_fp >> 16), ao1 = (float)(v1->ao_fp >> 16), ao2 = (float)(v2->ao_fp >> 16);

    float db0_dx = (sy1 - sy2) * inv_a, db1_dx = (sy2 - sy0) * inv_a;
    float db0_dy = (sx2 - sx1) * inv_a, db1_dy = (sx0 - sx2) * inv_a;
    float fx0 = (float)min_x + 0.5f, fy0 = (float)min_y + 0.5f;
    float b0_row = ((sx1 - fx0) * (sy2 - fy0) - (sy1 - fy0) * (sx2 - fx0)) * inv_a;
    float b1_row = ((sx2 - fx0) * (sy0 - fy0) - (sy2 - fy0) * (sx0 - fx0)) * inv_a;

    for (int py = min_y; py <= max_y; py++) {
        float b0 = b0_row, b1 = b1_row;
        bool seen = false;
        size_t roff = (size_t)py * (size_t)w;
        for (int px = min_x; px <= max_x; px++) {
            float b2 = 1.0f - b0 - b1;
            if (b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f) {
                seen = true;
                size_t idx = roff + (size_t)px;
                if (s_gbuf[idx].mat == 2u) {
                    float zf = b0 * z0 + b1 * z1 + b2 * z2;
                    uint16_t zv = (zf <= 0.0f) ? 0u : (zf >= 65535.0f ? 65534u : (uint16_t)zf);
                    if (zv < s_mirr_zbuf[idx]) {
                        s_mirr_zbuf[idx] = zv;
                        int nx = (int)(b0 * nx0 + b1 * nx1 + b2 * nx2);
                        int ny = (int)(b0 * ny0 + b1 * ny1 + b2 * ny2);
                        int nz = (int)(b0 * nz0 + b1 * nz1 + b2 * nz2);
                        int ao = (int)(b0 * ao0 + b1 * ao1 + b2 * ao2);
                        if (nx < -16383) nx = -16383; else if (nx > 16383) nx = 16383;
                        if (ny < -16383) ny = -16383; else if (ny > 16383) ny = 16383;
                        if (nz < -16383) nz = -16383; else if (nz > 16383) nz = 16383;
                        if (ao < 0) ao = 0; else if (ao > 255) ao = 255;
                        s_mirr_gbuf[idx].nx = (int16_t)nx;
                        s_mirr_gbuf[idx].ny = (int16_t)ny;
                        s_mirr_gbuf[idx].nz = (int16_t)nz;
                        s_mirr_gbuf[idx].ao = (uint8_t)ao;
                        s_mirr_gbuf[idx].valid = 1u;
                        if (px < *io_bx0) *io_bx0 = px;
                        if (px > *io_bx1) *io_bx1 = px;
                        if (py < *io_by0) *io_by0 = py;
                        if (py > *io_by1) *io_by1 = py;
                    }
                }
            } else if (seen) {
                break;
            }
            b0 += db0_dx;
            b1 += db1_dx;
        }
        b0_row += db0_dy;
        b1_row += db1_dy;
    }
}

typedef struct {
    int vp_w;
    int px0, px1;
    int mvc;
    int band_count;
    int band_y0[16];
    int band_y1[16];
    int l_bx0[16], l_by0[16], l_bx1[16], l_by1[16];
    volatile int next_band;
} mirror_band_job_t;

static void mirror_band_worker(int tid, void *ctx) {
    (void)tid;
    mirror_band_job_t *j = (mirror_band_job_t *)ctx;
    for (;;) {
        int b = __atomic_fetch_add(&j->next_band, 1, __ATOMIC_RELAXED);
        if (b >= j->band_count) break;
        int by0 = j->band_y0[b];
        int by1 = j->band_y1[b];
        int bx0 = j->px1 + 1, by0_l = by1 + 1, bx1 = j->px0 - 1, by1_l = by0 - 1;
        for (int i = 0; i < j->mvc; i++) {
            const vis_tri_t *vt = &s_vis_shd_tris[i];
            if ((int)vt->max_y < by0 || (int)vt->min_y > by1) continue;
            int idx = (int)vt->tri_idx * 3;
            raster_mirror_tri(j->vp_w, j->px0, by0, j->px1, by1,
                              &s_mverts[teapot_indices[idx + 0]],
                              &s_mverts[teapot_indices[idx + 1]],
                              &s_mverts[teapot_indices[idx + 2]],
                              &bx0, &by0_l, &bx1, &by1_l);
        }
        j->l_bx0[b] = bx0; j->l_by0[b] = by0_l;
        j->l_bx1[b] = bx1; j->l_by1[b] = by1_l;
    }
}

typedef struct {
    int shd_w, shd_h;
    int x0, x1;
    int y_end;
    volatile int next_y;
} blur_job_t;

static void blur_x_worker(int tid, void *ctx) {
    (void)tid;
    blur_job_t *j = (blur_job_t *)ctx;
    const int shd_w = j->shd_w, x0 = j->x0, x1 = j->x1, y_end = j->y_end;
    for (;;) {
        int y_start = __atomic_fetch_add(&j->next_y, 8, __ATOMIC_RELAXED);
        if (y_start > y_end) break;
        int y_stop = y_start + 7;
        if (y_stop > y_end) y_stop = y_end;
        for (int y = y_start; y <= y_stop; y++) {
            const uint8_t *src = s_shd_mask + (size_t)y * (size_t)shd_w;
            uint8_t *dst = s_shd_tmp + (size_t)y * (size_t)shd_w;
            for (int x = x0; x <= x1; x++) {
                uint32_t sum = (uint32_t)src[x - 2] + ((uint32_t)src[x - 1] << 2) +
                               ((uint32_t)src[x] * 6u) + ((uint32_t)src[x + 1] << 2) +
                               (uint32_t)src[x + 2];
                dst[x] = (uint8_t)(sum >> 4);
            }
        }
    }
}

static void blur_y_worker(int tid, void *ctx) {
    (void)tid;
    blur_job_t *j = (blur_job_t *)ctx;
    const int shd_w = j->shd_w, x0 = j->x0, x1 = j->x1, y_end = j->y_end;
    for (;;) {
        int y_start = __atomic_fetch_add(&j->next_y, 8, __ATOMIC_RELAXED);
        if (y_start > y_end) break;
        int y_stop = y_start + 7;
        if (y_stop > y_end) y_stop = y_end;
        for (int y = y_start; y <= y_stop; y++) {
            const uint8_t *rm2 = s_shd_tmp + (size_t)(y - 2) * (size_t)shd_w;
            const uint8_t *rm1 = s_shd_tmp + (size_t)(y - 1) * (size_t)shd_w;
            const uint8_t *r0  = s_shd_tmp + (size_t)y * (size_t)shd_w;
            const uint8_t *rp1 = s_shd_tmp + (size_t)(y + 1) * (size_t)shd_w;
            const uint8_t *rp2 = s_shd_tmp + (size_t)(y + 2) * (size_t)shd_w;
            uint8_t *dst = s_shd_blur + (size_t)y * (size_t)shd_w;
            for (int x = x0; x <= x1; x++) {
                uint32_t sum = (uint32_t)rm2[x] + ((uint32_t)rm1[x] << 2) +
                               ((uint32_t)r0[x] * 6u) + ((uint32_t)rp1[x] << 2) +
                               (uint32_t)rp2[x];
                dst[x] = (uint8_t)(sum >> 4);
            }
        }
    }
}

static void build_bands(raster_band_job_t *j, int min_y, int max_y, int w, int h, int vc) {
    j->w = w;
    j->h = h;
    j->vis_count = vc;
    j->band_y0 = NULL;
    j->band_y1 = NULL;
    j->band_count = 0;
    j->next_band = 0;
    int span = max_y - min_y + 1;
    if (span < 1) return;
    int workers = s_pool.worker_count + 1;
    int nb = workers * 2;
    if (nb > span) nb = span;
    if (nb < 1) nb = 1;
    if ((size_t)nb > SIZE_MAX / (2u * sizeof(int))) return;
    int *mem = (int *)malloc((size_t)nb * 2u * sizeof(int));
    if (!mem) return;
    j->band_y0 = mem;
    j->band_y1 = mem + nb;
    j->band_count = nb;
    int y = min_y;
    for (int b = 0; b < nb; b++) {
        int ny = min_y + ((span * (b + 1)) / nb);
        j->band_y0[b] = y;
        j->band_y1[b] = (b == nb - 1) ? max_y : (ny - 1);
        y = ny;
    }
}

static void free_bands(raster_band_job_t *j) {
    if (!j) return;
    free(j->band_y0);
    j->band_y0 = NULL;
    j->band_y1 = NULL;
    j->band_count = 0;
}

static void teapot_gpu_mat4_mul(float out[16], const float a[16], const float b[16]) {
    float r[16];
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            r[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] +
                               a[1 * 4 + row] * b[col * 4 + 1] +
                               a[2 * 4 + row] * b[col * 4 + 2] +
                               a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

static void teapot_gpu_transpose(const float cm[16], float rm[16]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) rm[r * 4 + c] = cm[c * 4 + r];
    }
}

static void teapot_gpu_build_ortho(float m[16], float l, float r, float b, float t, float n, float f) {
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / (r - l);
    m[5]  = 2.0f / (t - b);
    m[10] = -2.0f / (f - n);
    m[12] = -(r + l) / (r - l);
    m[13] = -(t + b) / (t - b);
    m[14] = -(f + n) / (f - n);
    m[15] = 1.0f;
}

static void teapot_gpu_build_lookat(float m[16], const float eye[3], const float at[3], const float up_in[3]) {
    float fx = at[0] - eye[0], fy = at[1] - eye[1], fz = at[2] - eye[2];
    float fl = gl_sqrtf(fx * fx + fy * fy + fz * fz);
    if (fl < 1e-6f) fl = 1.0f;
    fx /= fl; fy /= fl; fz /= fl;

    float sx = fy * up_in[2] - fz * up_in[1];
    float sy = fz * up_in[0] - fx * up_in[2];
    float sz = fx * up_in[1] - fy * up_in[0];
    float sl = gl_sqrtf(sx * sx + sy * sy + sz * sz);
    if (sl < 1e-6f) sl = 1.0f;
    sx /= sl; sy /= sl; sz /= sl;

    float ux = sy * fz - sz * fy;
    float uy = sz * fx - sx * fz;
    float uz = sx * fy - sy * fx;

    m[0] = sx;  m[4] = sy;  m[8]  = sz;  m[12] = -(sx * eye[0] + sy * eye[1] + sz * eye[2]);
    m[1] = ux;  m[5] = uy;  m[9]  = uz;  m[13] = -(ux * eye[0] + uy * eye[1] + uz * eye[2]);
    m[2] = -fx; m[6] = -fy; m[10] = -fz; m[14] =  (fx * eye[0] + fy * eye[1] + fz * eye[2]);
    m[3] = 0.0f; m[7] = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
}

static struct {
    uint32_t ctx_id;
    uint32_t hdr_msaa_res;
    uint32_t depth_msaa_res;
    uint32_t hdr_res;
    uint32_t depth_res;
    uint32_t bloom_a_res;
    uint32_t bloom_b_res;
    uint32_t shd_res;
    uint32_t sdr_res;
    uint32_t bg_tex_res;
    uint32_t hud_tex_res;
    uint32_t env_sharp_res;
    uint32_t env_soft_res;
    uint32_t tp_vbo;
    uint32_t tp_ibo;
    uint32_t tp_tri_count;
    uint32_t ped_vbo;
    uint32_t ped_vert_cnt;
    uint32_t trim_vbo;
    uint32_t trim_vert_cnt;
    ipo_gpu_dynbuf_t dyn2d_vbo;
    ipo_gpu_batch_t  batch;
    int      rt_w, rt_h, gl_h;
    int      bloom_w, bloom_h;
    uint32_t msaa_samples;
    int      hud_w, hud_h;
    char     *last_hud;
    uint32_t last_assets_ms;
    uint32_t last_flush_ms;
} s_tp_gpu;

static void teapot_gpu_destroy_rt(void) {
    if (s_tp_gpu.hdr_msaa_res)   { ipo_gpu_res_destroy(s_tp_gpu.hdr_msaa_res);   s_tp_gpu.hdr_msaa_res = 0u; }
    if (s_tp_gpu.depth_msaa_res) { ipo_gpu_res_destroy(s_tp_gpu.depth_msaa_res); s_tp_gpu.depth_msaa_res = 0u; }
    if (s_tp_gpu.hdr_res)        { ipo_gpu_res_destroy(s_tp_gpu.hdr_res);        s_tp_gpu.hdr_res = 0u; }
    if (s_tp_gpu.depth_res)      { ipo_gpu_res_destroy(s_tp_gpu.depth_res);      s_tp_gpu.depth_res = 0u; }
    if (s_tp_gpu.bloom_a_res)    { ipo_gpu_res_destroy(s_tp_gpu.bloom_a_res);    s_tp_gpu.bloom_a_res = 0u; }
    if (s_tp_gpu.bloom_b_res)    { ipo_gpu_res_destroy(s_tp_gpu.bloom_b_res);    s_tp_gpu.bloom_b_res = 0u; }
    if (s_tp_gpu.sdr_res)        { ipo_gpu_res_destroy(s_tp_gpu.sdr_res);        s_tp_gpu.sdr_res = 0u; }
    if (s_tp_gpu.bg_tex_res)     { ipo_gpu_res_destroy(s_tp_gpu.bg_tex_res);     s_tp_gpu.bg_tex_res = 0u; }
    if (s_tp_gpu.hud_tex_res)    { ipo_gpu_res_destroy(s_tp_gpu.hud_tex_res);    s_tp_gpu.hud_tex_res = 0u; }
    s_tp_gpu.rt_w = 0; s_tp_gpu.rt_h = 0; s_tp_gpu.gl_h = 0;
    s_tp_gpu.bloom_w = 0; s_tp_gpu.bloom_h = 0; s_tp_gpu.msaa_samples = 0u;
    free(s_tp_gpu.last_hud);
    s_tp_gpu.last_hud = NULL;
}

static void teapot_gpu_destroy(void) {
    teapot_gpu_destroy_rt();
    if (s_tp_gpu.env_sharp_res) { ipo_gpu_res_destroy(s_tp_gpu.env_sharp_res); s_tp_gpu.env_sharp_res = 0u; }
    if (s_tp_gpu.env_soft_res)  { ipo_gpu_res_destroy(s_tp_gpu.env_soft_res);  s_tp_gpu.env_soft_res = 0u; }
    if (s_tp_gpu.shd_res)  { ipo_gpu_res_destroy(s_tp_gpu.shd_res);  s_tp_gpu.shd_res = 0u; }
    if (s_tp_gpu.tp_vbo)   { ipo_gpu_res_destroy(s_tp_gpu.tp_vbo);   s_tp_gpu.tp_vbo = 0u; }
    if (s_tp_gpu.tp_ibo)   { ipo_gpu_res_destroy(s_tp_gpu.tp_ibo);   s_tp_gpu.tp_ibo = 0u; }
    if (s_tp_gpu.ped_vbo)  { ipo_gpu_res_destroy(s_tp_gpu.ped_vbo);  s_tp_gpu.ped_vbo = 0u; }
    if (s_tp_gpu.trim_vbo) { ipo_gpu_res_destroy(s_tp_gpu.trim_vbo); s_tp_gpu.trim_vbo = 0u; }
    ipo_gpu_dynbuf_destroy(&s_tp_gpu.dyn2d_vbo);
    ipo_gpu_batch_free(&s_tp_gpu.batch);
    if (s_tp_gpu.ctx_id)  { ipo_gpu_ctx_destroy(s_tp_gpu.ctx_id);  s_tp_gpu.ctx_id = 0u; }
}

static bool teapot_gpu_build_subdivided_mesh(void) {
    if (!s_sub_verts || !s_sub_idx || s_sub_vcount == 0u || s_sub_tcount == 0u) {
        return false;
    }
    ipo_gpu_vertex_t *tv = (ipo_gpu_vertex_t *)malloc((size_t)s_sub_vcount * sizeof(ipo_gpu_vertex_t));
    if (!tv) return false;
    for (uint32_t i = 0u; i < s_sub_vcount; i++) {
        const tp_subv_t *s = &s_sub_verts[i];
        float ao = (float)s->ao / 255.0f;
        tv[i] = (ipo_gpu_vertex_t){ s->px, s->py, s->pz, s->nx, s->ny, s->nz, 0.0f, 0.0f, ao, ao, ao, 1.0f };
    }
    uint32_t sub_i_cnt = s_sub_tcount * 3u;
    s_tp_gpu.tp_vbo = ipo_gpu_res_create_buf(s_tp_gpu.ctx_id, IPO_GPU_RES_VERTEX_BUF,
                                             tv, s_sub_vcount * (uint32_t)sizeof(ipo_gpu_vertex_t));
    s_tp_gpu.tp_ibo = ipo_gpu_res_create_buf(s_tp_gpu.ctx_id, IPO_GPU_RES_INDEX_BUF,
                                             s_sub_idx, sub_i_cnt * (uint32_t)sizeof(uint32_t));
    s_tp_gpu.tp_tri_count = s_sub_tcount;
    free(tv);
    return (s_tp_gpu.tp_vbo != 0u && s_tp_gpu.tp_ibo != 0u);
}

static bool teapot_gpu_ensure_assets(int ren_w, int gl_h, int ren_h) {
    if (!s_tp_gpu.ctx_id) {
        s_tp_gpu.ctx_id = ipo_gpu_ctx_create("gl_teapot");
        if (!s_tp_gpu.ctx_id) return false;
    }
    if (!s_tp_gpu.env_sharp_res || !s_tp_gpu.env_soft_res) {
        (void)ipo_gpu_create_studio_env(s_tp_gpu.ctx_id, &s_tp_gpu.env_sharp_res, &s_tp_gpu.env_soft_res);
    }
    if (!s_tp_gpu.shd_res) {
        s_tp_gpu.shd_res = ipo_gpu_res_create_rt(s_tp_gpu.ctx_id, IPO_GPU_FMT_Z32F, 1024u, 1024u,
                                                 IPO_GPU_RES_FLAG_SAMPLER);
        if (!s_tp_gpu.shd_res) return false;
    }
    if (!s_tp_gpu.tp_vbo) {
        init_teapot_data();
        if (!s_mesh_inited || !s_teapot_ao || !s_teapot_normals) return false;
        if (!teapot_gpu_build_subdivided_mesh()) return false;
    }
    if (!s_tp_gpu.ped_vbo) {
        typedef struct { float x, y, z, nx, ny, nz, ao; } ped_v_t;
        ped_v_t vc = { 0.0f, -0.912f, 0.0f, 0.0f, 1.0f, 0.0f, 0.24f };
        ped_v_t *rings = (ped_v_t *)malloc((size_t)(PED_SEGS + 1) * 4u * sizeof(ped_v_t));
        uint32_t nverts = (uint32_t)(PED_SEGS * 7 * 3);
        ipo_gpu_vertex_t *pv = (ipo_gpu_vertex_t *)malloc((size_t)nverts * sizeof(ipo_gpu_vertex_t));
        if (!rings || !pv) { free(rings); free(pv); return false; }
        ped_v_t *vmid = &rings[0 * (PED_SEGS + 1)];
        ped_v_t *rtop = &rings[1 * (PED_SEGS + 1)];
        ped_v_t *rbev = &rings[2 * (PED_SEGS + 1)];
        ped_v_t *rbot = &rings[3 * (PED_SEGS + 1)];
        for (int i = 0; i <= PED_SEGS; i++) {
            float ang = ((float)(i % PED_SEGS) / (float)PED_SEGS) * 6.2831853f;
            float ca = gl_cosf(ang), sa = gl_sinf(ang);
            vmid[i] = (ped_v_t){ ca * 1.05f, -0.912f, sa * 1.05f, 0.0f, 1.0f, 0.0f, 0.38f };
            rtop[i] = (ped_v_t){ ca * 2.62f, -0.912f, sa * 2.62f, 0.0f, 1.0f, 0.0f, 0.96f };
            rbev[i] = (ped_v_t){ ca * 2.75f, -0.962f, sa * 2.75f, ca * 0.7071f, 0.7071f, sa * 0.7071f, 1.00f };
            rbot[i] = (ped_v_t){ ca * 2.75f, -1.200f, sa * 2.75f, ca, 0.0f, sa, 0.72f };
        }
        uint32_t k = 0u;
        #define EMIT_PED_V(p) do { \
            pv[k++] = (ipo_gpu_vertex_t){ (p).x, (p).y, (p).z, (p).nx, (p).ny, (p).nz, 0.0f, 0.0f, (p).ao, (p).ao, (p).ao, 1.0f }; \
        } while (0)
        #define EMIT_PED_TRI(a, b, c) do { EMIT_PED_V(a); EMIT_PED_V(b); EMIT_PED_V(c); } while (0)
        /* Contiguous top disc first (0 .. PED_SEGS * 9) for mirror masking & translucent obsidian surface */
        for (int i = 0; i < PED_SEGS; i++) {
            EMIT_PED_TRI(vc, vmid[i], vmid[i + 1]);
            EMIT_PED_TRI(vmid[i], rtop[i], vmid[i + 1]);
            EMIT_PED_TRI(vmid[i + 1], rtop[i], rtop[i + 1]);
        }
        for (int i = 0; i < PED_SEGS; i++) {
            EMIT_PED_TRI(rtop[i], rbev[i], rtop[i + 1]);
            EMIT_PED_TRI(rtop[i + 1], rbev[i], rbev[i + 1]);
            EMIT_PED_TRI(rbev[i], rbot[i], rbev[i + 1]);
            EMIT_PED_TRI(rbev[i + 1], rbot[i], rbot[i + 1]);
        }
        #undef EMIT_PED_V
        #undef EMIT_PED_TRI
        s_tp_gpu.ped_vbo = ipo_gpu_res_create_buf(s_tp_gpu.ctx_id, IPO_GPU_RES_VERTEX_BUF,
                                                  pv, k * (uint32_t)sizeof(ipo_gpu_vertex_t));
        s_tp_gpu.ped_vert_cnt = k;
        free(rings); free(pv);
        if (!s_tp_gpu.ped_vbo) return false;
    }
    if (!s_tp_gpu.trim_vbo) {
        /* Champagne gold metallic precision inlay ring embedded around turntable edge */
        uint32_t tverts = (uint32_t)(PED_SEGS * 6 * 3);
        ipo_gpu_vertex_t *tv = (ipo_gpu_vertex_t *)malloc((size_t)tverts * sizeof(ipo_gpu_vertex_t));
        if (!tv) return false;
        uint32_t tk = 0u;
        for (int i = 0; i < PED_SEGS; i++) {
            float a0 = ((float)i / (float)PED_SEGS) * 6.2831853f;
            float a1 = ((float)(i + 1) / (float)PED_SEGS) * 6.2831853f;
            float c0 = gl_cosf(a0), s0 = gl_sinf(a0);
            float c1 = gl_cosf(a1), s1 = gl_sinf(a1);
            ipo_gpu_vertex_t p0a = { c0 * 2.385f, -0.9125f, s0 * 2.385f, -c0 * 0.707f, 0.707f, -s0 * 0.707f, 0,0, 0.85f,0.85f,0.85f,1.0f };
            ipo_gpu_vertex_t p1a = { c1 * 2.385f, -0.9125f, s1 * 2.385f, -c1 * 0.707f, 0.707f, -s1 * 0.707f, 0,0, 0.85f,0.85f,0.85f,1.0f };
            ipo_gpu_vertex_t p0b = { c0 * 2.400f, -0.9075f, s0 * 2.400f,  0.0f,        1.0f,    0.0f,        0,0, 1.0f, 1.0f, 1.0f, 1.0f };
            ipo_gpu_vertex_t p1b = { c1 * 2.400f, -0.9075f, s1 * 2.400f,  0.0f,        1.0f,    0.0f,        0,0, 1.0f, 1.0f, 1.0f, 1.0f };
            ipo_gpu_vertex_t p0c = { c0 * 2.445f, -0.9075f, s0 * 2.445f,  0.0f,        1.0f,    0.0f,        0,0, 1.0f, 1.0f, 1.0f, 1.0f };
            ipo_gpu_vertex_t p1c = { c1 * 2.445f, -0.9075f, s1 * 2.445f,  0.0f,        1.0f,    0.0f,        0,0, 1.0f, 1.0f, 1.0f, 1.0f };
            ipo_gpu_vertex_t p0d = { c0 * 2.460f, -0.9125f, s0 * 2.460f,  c0 * 0.707f, 0.707f,  s0 * 0.707f, 0,0, 0.88f,0.88f,0.88f,1.0f };
            ipo_gpu_vertex_t p1d = { c1 * 2.460f, -0.9125f, s1 * 2.460f,  c1 * 0.707f, 0.707f,  s1 * 0.707f, 0,0, 0.88f,0.88f,0.88f,1.0f };
            tv[tk++] = p0a; tv[tk++] = p0b; tv[tk++] = p1a;
            tv[tk++] = p1a; tv[tk++] = p0b; tv[tk++] = p1b;
            tv[tk++] = p0b; tv[tk++] = p0c; tv[tk++] = p1b;
            tv[tk++] = p1b; tv[tk++] = p0c; tv[tk++] = p1c;
            tv[tk++] = p0c; tv[tk++] = p0d; tv[tk++] = p1c;
            tv[tk++] = p1c; tv[tk++] = p0d; tv[tk++] = p1d;
        }
        s_tp_gpu.trim_vbo = ipo_gpu_res_create_buf(s_tp_gpu.ctx_id, IPO_GPU_RES_VERTEX_BUF,
                                                   tv, tk * (uint32_t)sizeof(ipo_gpu_vertex_t));
        s_tp_gpu.trim_vert_cnt = tk;
        free(tv);
    }
    if (!s_tp_gpu.hdr_res || !s_tp_gpu.depth_res || !s_tp_gpu.sdr_res ||
        !s_tp_gpu.bg_tex_res || s_tp_gpu.rt_w != ren_w || s_tp_gpu.rt_h != ren_h || s_tp_gpu.gl_h != gl_h) {
        teapot_gpu_destroy_rt();
        uint32_t max_samp = ipo_gpu_max_samples();
        (void)max_samp;
        /* Perceptual FXAA in FS_TONEMAP already smooths edges without 2x llvmpipe MSAA overhead */
        uint32_t want_msaa = 0u;
        if (want_msaa >= 2u) {
            s_tp_gpu.hdr_msaa_res   = ipo_gpu_res_create_rt(s_tp_gpu.ctx_id, IPO_GPU_FMT_RGBA16F,
                                                            (uint32_t)ren_w, (uint32_t)ren_h,
                                                            IPO_GPU_RES_FLAG_MSAA(want_msaa));
            s_tp_gpu.depth_msaa_res = ipo_gpu_res_create_rt(s_tp_gpu.ctx_id, IPO_GPU_FMT_Z32F,
                                                            (uint32_t)ren_w, (uint32_t)ren_h,
                                                            IPO_GPU_RES_FLAG_MSAA(want_msaa));
            if (s_tp_gpu.hdr_msaa_res && s_tp_gpu.depth_msaa_res) {
                s_tp_gpu.msaa_samples = want_msaa;
            } else {
                if (s_tp_gpu.hdr_msaa_res)   { ipo_gpu_res_destroy(s_tp_gpu.hdr_msaa_res);   s_tp_gpu.hdr_msaa_res = 0u; }
                if (s_tp_gpu.depth_msaa_res) { ipo_gpu_res_destroy(s_tp_gpu.depth_msaa_res); s_tp_gpu.depth_msaa_res = 0u; }
            }
        }
        s_tp_gpu.hdr_res   = ipo_gpu_res_create_rt(s_tp_gpu.ctx_id, IPO_GPU_FMT_RGBA16F,
                                                   (uint32_t)ren_w, (uint32_t)ren_h, IPO_GPU_RES_FLAG_SAMPLER);
        s_tp_gpu.depth_res = ipo_gpu_res_create_rt(s_tp_gpu.ctx_id, IPO_GPU_FMT_Z32F,
                                                   (uint32_t)ren_w, (uint32_t)ren_h, 0u);
        s_tp_gpu.bloom_w = (ren_w / 4 > 32) ? (ren_w / 4) : 32;
        s_tp_gpu.bloom_h = (ren_h / 4 > 24) ? (ren_h / 4) : 24;
        s_tp_gpu.bloom_a_res = ipo_gpu_res_create_rt(s_tp_gpu.ctx_id, IPO_GPU_FMT_RGBA16F,
                                                     (uint32_t)s_tp_gpu.bloom_w, (uint32_t)s_tp_gpu.bloom_h,
                                                     IPO_GPU_RES_FLAG_SAMPLER);
        s_tp_gpu.bloom_b_res = ipo_gpu_res_create_rt(s_tp_gpu.ctx_id, IPO_GPU_FMT_RGBA16F,
                                                     (uint32_t)s_tp_gpu.bloom_w, (uint32_t)s_tp_gpu.bloom_h,
                                                     IPO_GPU_RES_FLAG_SAMPLER);
        s_tp_gpu.sdr_res   = ipo_gpu_res_create_rt(s_tp_gpu.ctx_id, IPO_GPU_FMT_BGRX8,
                                                   (uint32_t)ren_w, (uint32_t)ren_h, IPO_GPU_RES_FLAG_SAMPLER);
        if (!s_tp_gpu.hdr_res || !s_tp_gpu.depth_res || !s_tp_gpu.sdr_res) return false;

        /* Bake radial studio cyclorama halo into a bilinear-filtered RGBA32F GPU texture */
        if (!s_tp_gpu.bg_tex_res) {
            const int bg_w = 160, bg_h = 120;
            float *bgf = (float *)malloc((size_t)bg_w * (size_t)bg_h * 4u * sizeof(float));
            if (!bgf) return false;
            float cx = (float)bg_w * 0.5f, cy = (float)bg_h * 0.43f;
            float inv_r2 = 1.0f / ((float)bg_w * (float)bg_w * 0.34f + (float)bg_h * (float)bg_h * 0.34f);
            for (int y = 0; y < bg_h; y++) {
                float dy = (float)y - cy, dy2 = dy * dy;
                float grad_y = 1.0f - (float)y / (float)bg_h;
                for (int x = 0; x < bg_w; x++) {
                    float dx = (float)x - cx;
                    float halo = 1.0f - (dx * dx + dy2) * inv_r2;
                    if (halo < 0.0f) halo = 0.0f;
                    halo = halo * halo * (1.25f - 0.25f * halo);
                    float lum = 0.032f + halo * 0.145f + grad_y * 0.022f;
                    size_t idx = ((size_t)y * (size_t)bg_w + (size_t)x) * 4u;
                    bgf[idx + 0u] = lum * 0.96f; bgf[idx + 1u] = lum * 0.99f;
                    bgf[idx + 2u] = lum * 1.05f; bgf[idx + 3u] = 1.0f;
                }
            }
            s_tp_gpu.bg_tex_res = ipo_gpu_res_create_tex2d(s_tp_gpu.ctx_id, IPO_GPU_FMT_RGBA32F,
                                                           (uint32_t)bg_w, (uint32_t)bg_h,
                                                           bgf, (uint32_t)bg_w * 16u);
            free(bgf);
            if (!s_tp_gpu.bg_tex_res) return false;
        }

        s_tp_gpu.rt_w = ren_w;
        s_tp_gpu.rt_h = ren_h;
        s_tp_gpu.gl_h = gl_h;
    }
    return true;
}

static void teapot_gpu_sync_hud(int ren_w, int bar_h, const char *text) {
    if (bar_h <= 0 || ren_w <= 0 || !text) return;
    if (s_tp_gpu.hud_tex_res && s_tp_gpu.hud_w == ren_w && s_tp_gpu.hud_h == bar_h &&
        s_tp_gpu.last_hud && strcmp(s_tp_gpu.last_hud, text) == 0) {
        return;
    }
    ipo_rgba64f_t *h64 = (ipo_rgba64f_t *)calloc((size_t)ren_w * (size_t)bar_h, sizeof(ipo_rgba64f_t));
    uint32_t *h32 = (uint32_t *)malloc((size_t)ren_w * (size_t)bar_h * sizeof(uint32_t));
    if (!h64 || !h32) { free(h64); free(h32); return; }

    ipo_gfx3d_ctx_t tmp_ctx;
    memset(&tmp_ctx, 0, sizeof(tmp_ctx));
    tmp_ctx.width = ren_w; tmp_ctx.height = bar_h; tmp_ctx.color_fb = h64;
    ipo_gfx3d_draw_rect2d(&tmp_ctx, 0, 0, ren_w, bar_h, (ipo_rgba64f_t){ 0.04, 0.05, 0.08, 1.0 });
    ipo_gfx3d_draw_rect2d(&tmp_ctx, 0, 0, ren_w, 1, (ipo_rgba64f_t){ 0.25, 0.30, 0.42, 1.0 });
    ipo_gfx3d_draw_text2d(&tmp_ctx, 6, 4, text, (ipo_rgba64f_t){ 0.95, 0.97, 1.0, 1.0 }, 1);

    size_t np = (size_t)ren_w * (size_t)bar_h;
    for (size_t i = 0; i < np; i++) {
        double r = h64[i].r; if (r < 0.0) r = 0.0; else if (r > 1.0) r = 1.0;
        double g = h64[i].g; if (g < 0.0) g = 0.0; else if (g > 1.0) g = 1.0;
        double b = h64[i].b; if (b < 0.0) b = 0.0; else if (b > 1.0) b = 1.0;
        uint32_t r8 = (uint32_t)(r * 255.0 + 0.5);
        uint32_t g8 = (uint32_t)(g * 255.0 + 0.5);
        uint32_t b8 = (uint32_t)(b * 255.0 + 0.5);
        h32[i] = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
    }
    free(h64);

    if (!s_tp_gpu.hud_tex_res || s_tp_gpu.hud_w != ren_w || s_tp_gpu.hud_h != bar_h) {
        if (s_tp_gpu.hud_tex_res) ipo_gpu_res_destroy(s_tp_gpu.hud_tex_res);
        s_tp_gpu.hud_tex_res = ipo_gpu_res_create_tex2d(s_tp_gpu.ctx_id, IPO_GPU_FMT_BGRA8,
                                                        (uint32_t)ren_w, (uint32_t)bar_h,
                                                        h32, (uint32_t)ren_w * 4u);
        s_tp_gpu.hud_w = ren_w;
        s_tp_gpu.hud_h = bar_h;
    } else {
        ipo_gpu_res_upload_tex2d(s_tp_gpu.ctx_id, s_tp_gpu.hud_tex_res, 0u, 0u,
                                 (uint32_t)ren_w, (uint32_t)bar_h, (uint32_t)ren_w * 4u, h32);
    }
    free(h32);
    size_t tlen = strlen(text);
    char *copy = (char *)realloc(s_tp_gpu.last_hud, tlen + 1u);
    if (copy) {
        memcpy(copy, text, tlen + 1u);
        s_tp_gpu.last_hud = copy;
    }
}

static bool render_scene_gpu(wm_window_t *win, int ren_w, int gl_h, int ren_h,
                             int frame_mat, int current_fps, bool hd_fs,
                             bool mouse_dragging) {
    uint32_t t_a0 = timer_millis();
    if (!ipo_gpu_3d_available() || !teapot_gpu_ensure_assets(ren_w, gl_h, ren_h)) {
        return false;
    }
    uint32_t t_a1 = timer_millis();
    s_tp_gpu.last_assets_ms = t_a1 - t_a0;

    GLfloat mv_base[16], pj[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv_base);
    glGetFloatv(GL_PROJECTION_MATRIX, pj);

    float ry_rad = rot_y * (3.14159265f / 180.0f);
    float rx_rad = rot_x * (3.14159265f / 180.0f);
    float cry = gl_cosf(ry_rad), sry = gl_sinf(ry_rad);
    float model_tp[16] = {
         cry, 0.0f, -sry, 0.0f,
         0.0f, 1.0f, 0.0f, 0.0f,
         sry, 0.0f,  cry, 0.0f,
         0.0f, 0.0f, 0.0f, 1.0f
    };
    float model_id[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    float vp[16], mv_tp[16], mvp_tp[16];
    teapot_gpu_mat4_mul(vp, pj, mv_base);
    teapot_gpu_mat4_mul(mv_tp, mv_base, model_tp);
    teapot_gpu_mat4_mul(mvp_tp, pj, mv_tp);

    /* Camera position in world space (inverse of mv_base = Rx(-rot_x) * T(0, +0.02, +cam_dist)) */
    float crx = gl_cosf(rx_rad), srx = gl_sinf(rx_rad);
    float cam_wx = 0.0f;
    float cam_wy = 0.02f * crx + cam_dist * srx;
    float cam_wz = -0.02f * srx + cam_dist * crx;

    /* Shadow Light View-Projection matrix aligned with Key Light */
    float l0_dir[3] = { 0.55f, 0.78f, 0.44f };
    float l_eye[3]  = { l0_dir[0] * 7.5f, l0_dir[1] * 7.5f, l0_dir[2] * 7.5f };
    float l_at[3]   = { 0.0f, -0.15f, 0.0f };
    float l_up[3]   = { 0.0f, 1.0f, 0.0f };
    float l_view[16], l_proj[16], l_vp[16], l_vp_tp[16];
    teapot_gpu_build_lookat(l_view, l_eye, l_at, l_up);
    teapot_gpu_build_ortho(l_proj, -3.2f, 3.2f, -3.2f, 3.2f, 1.0f, 16.0f);
    teapot_gpu_mat4_mul(l_vp, l_proj, l_view);
    teapot_gpu_mat4_mul(l_vp_tp, l_vp, model_tp);

    ipo_gpu_batch_reset(&s_tp_gpu.batch);

    /* Pack 2D quads (bg quad, tonemap quad, hud quad, optional crosshair cursor) into dyn2d_vbo */
    float gl_y_ndc_bot = 1.0f - ((float)gl_h / (float)ren_h) * 2.0f;
    ipo_gpu_vertex_t v2d[48];
    memset(v2d, 0, sizeof(v2d));
    #define SET_2D_Z(idx, px0, py0, pz0, u0, v0, cr, cg, cb) do { \
        v2d[idx].px = (px0); v2d[idx].py = (py0); v2d[idx].pz = (pz0); \
        v2d[idx].u = (u0); v2d[idx].v = (v0); \
        v2d[idx].r = (cr); v2d[idx].g = (cg); v2d[idx].b = (cb); v2d[idx].a = 1.0f; \
    } while (0)
    #define SET_2D(idx, px0, py0, u0, v0, cr, cg, cb) SET_2D_Z((idx), (px0), (py0), 0.0f, (u0), (v0), (cr), (cg), (cb))
    /* 0..5: backdrop quad at Z_ndc = -0.90 (depth = 0.05) for stencil-free mirror clipping */
    SET_2D_Z(0, -1.0f, 1.0f,         -0.90f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    SET_2D_Z(1, -1.0f, gl_y_ndc_bot, -0.90f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    SET_2D_Z(2,  1.0f, 1.0f,         -0.90f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    SET_2D_Z(3,  1.0f, 1.0f,         -0.90f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    SET_2D_Z(4, -1.0f, gl_y_ndc_bot, -0.90f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    SET_2D_Z(5,  1.0f, gl_y_ndc_bot, -0.90f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    /* 6..11: fullscreen tonemap quad */
    SET_2D(6,  -1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    SET_2D(7,  -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    SET_2D(8,   1.0f,  1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    SET_2D(9,   1.0f,  1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    SET_2D(10, -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    SET_2D(11,  1.0f, -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    uint32_t v2d_cnt = 12u;

    bool have_hud = (ren_h > gl_h);
    uint32_t hud_v_start = v2d_cnt;
    if (have_hud) {
        char *status_buf = tp_alloc_fmt(
            "%d FPS | %s | VirGL GPU | %dx%d | M:Mat S:%s L:%s F11:%s",
            current_fps, materials[frame_mat].name, ren_w, ren_h,
            shadows_enabled ? "On" : "Off",
            light1_enabled ? "2L" : "1L",
            hd_fs ? "Win" : "FS");
        if (status_buf) {
            teapot_gpu_sync_hud(ren_w, ren_h - gl_h, status_buf);
            free(status_buf);
        }
        SET_2D(v2d_cnt + 0u, -1.0f, gl_y_ndc_bot, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        SET_2D(v2d_cnt + 1u, -1.0f, -1.0f,        0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        SET_2D(v2d_cnt + 2u,  1.0f, gl_y_ndc_bot, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        SET_2D(v2d_cnt + 3u,  1.0f, gl_y_ndc_bot, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        SET_2D(v2d_cnt + 4u, -1.0f, -1.0f,        0.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        SET_2D(v2d_cnt + 5u,  1.0f, -1.0f,        1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        v2d_cnt += 6u;
    }

    uint32_t cur_v_start = v2d_cnt;
    uint32_t cur_v_cnt = 0u;
    if (wm_get_focused() == win) {
        mouse_state_t cur_ms;
        mouse_get_state(&cur_ms);
        int lx = (int)cur_ms.x - (int)win->x;
        int ly = (int)cur_ms.y - (int)win->y;
        if (lx >= 0 && lx < (int)win->w && ly >= 0 && ly < (int)win->h) {
            int px = (lx * ren_w) / (win->w > 0 ? (int)win->w : 1);
            int py = (ly * ren_h) / (win->h > 0 ? (int)win->h : 1);
            float cr = mouse_dragging ? 1.0f : 0.25f;
            float cg = mouse_dragging ? 0.78f : 0.90f;
            float cb = mouse_dragging ? 0.12f : 1.0f;
            float hcx = ((float)px + 0.5f) / (float)ren_w * 2.0f - 1.0f;
            float hcy = 1.0f - ((float)py + 0.5f) / (float)ren_h * 2.0f;
            float armx = 7.0f / (float)ren_w * 2.0f;
            float army = 7.0f / (float)ren_h * 2.0f;
            float gapx = 2.0f / (float)ren_w * 2.0f;
            float gapy = 2.0f / (float)ren_h * 2.0f;
            float thx  = 1.2f / (float)ren_w * 2.0f;
            float thy  = 1.2f / (float)ren_h * 2.0f;
            #define EMIT_RECT(x0, y0, x1, y1) do { \
                SET_2D(v2d_cnt + 0u, (x0), (y0), 0, 0, cr, cg, cb); \
                SET_2D(v2d_cnt + 1u, (x0), (y1), 0, 0, cr, cg, cb); \
                SET_2D(v2d_cnt + 2u, (x1), (y0), 0, 0, cr, cg, cb); \
                SET_2D(v2d_cnt + 3u, (x1), (y0), 0, 0, cr, cg, cb); \
                SET_2D(v2d_cnt + 4u, (x0), (y1), 0, 0, cr, cg, cb); \
                SET_2D(v2d_cnt + 5u, (x1), (y1), 0, 0, cr, cg, cb); \
                v2d_cnt += 6u; cur_v_cnt += 6u; \
            } while (0)
            EMIT_RECT(hcx - armx, hcy + thy, hcx - gapx, hcy - thy);
            EMIT_RECT(hcx + gapx, hcy + thy, hcx + armx, hcy - thy);
            EMIT_RECT(hcx - thx, hcy + army, hcx + thx, hcy + gapy);
            EMIT_RECT(hcx - thx, hcy - gapy, hcx + thx, hcy - army);
            #undef EMIT_RECT
        }
    }
    #undef SET_2D
    #undef SET_2D_Z

    if (!ipo_gpu_dynbuf_upload(s_tp_gpu.ctx_id, &s_tp_gpu.dyn2d_vbo, IPO_GPU_RES_VERTEX_BUF,
                               v2d, v2d_cnt * (uint32_t)sizeof(ipo_gpu_vertex_t))) {
        return false;
    }

    /* Pass 1: Shadow Map Depth Pass */
    if (shadows_enabled) {
        ipo_gpu_cmd_set_target(&s_tp_gpu.batch, 0u, s_tp_gpu.shd_res, 1024u, 1024u);
        ipo_gpu_cmd_set_viewport(&s_tp_gpu.batch, 0.0f, 0.0f, 1024.0f, 1024.0f, 0.0f, 1.0f);
        float cz[4] = { 1, 1, 1, 1 };
        ipo_gpu_cmd_clear(&s_tp_gpu.batch, IPO_GPU_CLEAR_DEPTH, cz, 1.0f);
        ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_SHADOW,
                                 IPO_GPU_CULL_NONE, true, true, true, IPO_GPU_CMP_LESS, IPO_GPU_BLEND_NONE, false);
        float shd_vs[16];
        teapot_gpu_transpose(l_vp_tp, shd_vs);
        ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 0u, shd_vs, 16u);
        ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.tp_vbo, s_tp_gpu.tp_ibo, 4u,
                         IPO_GPU_PRIM_TRIANGLES, 0u, s_tp_gpu.tp_tri_count * 3u, 0u, 0u);
    }

    /* Pass 2: MSAA HDR Scene with Stencil-Free Planar Mirror Reflection on Obsidian Turntable */
    bool msaa_on = (s_tp_gpu.msaa_samples >= 2u && s_tp_gpu.hdr_msaa_res && s_tp_gpu.depth_msaa_res);
    uint32_t hdr_target   = msaa_on ? s_tp_gpu.hdr_msaa_res   : s_tp_gpu.hdr_res;
    uint32_t depth_target = msaa_on ? s_tp_gpu.depth_msaa_res : s_tp_gpu.depth_res;
    const uint32_t ped_top_verts = (uint32_t)PED_SEGS * 9u;
    const uint32_t ped_side_verts = (s_tp_gpu.ped_vert_cnt > ped_top_verts) ? (s_tp_gpu.ped_vert_cnt - ped_top_verts) : 0u;
    bool have_env = (s_tp_gpu.env_sharp_res != 0u && s_tp_gpu.env_soft_res != 0u);

    ipo_gpu_cmd_set_target(&s_tp_gpu.batch, hdr_target, depth_target,
                           (uint32_t)ren_w, (uint32_t)ren_h);
    ipo_gpu_cmd_set_viewport(&s_tp_gpu.batch, 0.0f, 0.0f, (float)ren_w, (float)ren_h, 0.0f, 1.0f);
    float cclr[4] = { 0.025f, 0.028f, 0.036f, 1.0f };
    ipo_gpu_cmd_clear(&s_tp_gpu.batch, IPO_GPU_CLEAR_COLOR | IPO_GPU_CLEAR_DEPTH, cclr, 0.0f);

    float vs_c[60];
    float fs_c[23u * 4u];
    memset(fs_c, 0, sizeof(fs_c));
    fs_c[0] = cam_wx; fs_c[1] = cam_wy; fs_c[2] = cam_wz; fs_c[3] = 0.0f;
    fs_c[4] = 0.16f;  fs_c[5] = 0.24f;  fs_c[6] = 0.55f;  fs_c[7] = 1.0f;
    fs_c[8] = 0.032f; fs_c[9] = 0.036f; fs_c[10] = 0.045f; fs_c[11] = 0.0f;
    fs_c[15] = 0.0f;
    fs_c[16] = 0.16f; fs_c[17] = 0.19f; fs_c[18] = 0.24f; fs_c[19] = 0.0f;
    fs_c[20] = 0.04f; fs_c[21] = 0.04f; fs_c[22] = 0.06f; fs_c[23] = 0.0012f;
    fs_c[88] = have_env ? 1.0f : 0.0f;
    fs_c[89] = 1.35f / 1024.0f;
    fs_c[90] = 0.0f;
    fs_c[91] = 0.14f;

    /* 2A: Write depth = 0.98f inside top disc (Z_clip = 0.96 * W_clip), dark studio base underneath mirror */
    ipo_gpu_cmd_set_viewport(&s_tp_gpu.batch, 0.0f, 0.0f, (float)ren_w, (float)gl_h, 0.0f, 1.0f);
    ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_PBR3D,
                             IPO_GPU_CULL_NONE, true, true, true, IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
    teapot_gpu_transpose(vp,       &vs_c[0]);
    teapot_gpu_transpose(model_id, &vs_c[16]);
    teapot_gpu_transpose(l_vp,     &vs_c[32]);
    vs_c[8]  = vs_c[12] * 0.96f; vs_c[9]  = vs_c[13] * 0.96f;
    vs_c[10] = vs_c[14] * 0.96f; vs_c[11] = vs_c[15] * 0.96f;
    memset(&vs_c[48], 0, 12u * sizeof(float));
    vs_c[48] = 1.0f; vs_c[53] = 1.0f; vs_c[58] = 1.0f;
    ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 0u, vs_c, 60u);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 0u, 0u, false, false);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 1u, 0u, false, false);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 2u, s_tp_gpu.env_sharp_res, true, false);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 3u, s_tp_gpu.env_soft_res, true, false);
    ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, fs_c, 92u);
    ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.ped_vbo, 0u, 0u,
                     IPO_GPU_PRIM_TRIANGLES, 0u, ped_top_verts, 0u, 0u);

    /* 2B: Draw radial studio backdrop at depth = 0.05f with CMP_GREATER (writes ONLY outside disc) */
    ipo_gpu_cmd_set_viewport(&s_tp_gpu.batch, 0.0f, 0.0f, (float)ren_w, (float)ren_h, 0.0f, 1.0f);
    ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_QUAD2D,
                             IPO_GPU_CULL_NONE, true, true, true, IPO_GPU_CMP_GREATER, IPO_GPU_BLEND_NONE, false);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 0u, s_tp_gpu.bg_tex_res, true, false);
    ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.dyn2d_vbo.res_id, 0u, 0u, IPO_GPU_PRIM_TRIANGLES, 0u, 6u, 0u, 0u);

    /* 2C: Render Mirrored PN-Teapot across y = -0.912 (CMP_LESS passes strictly inside 0.98f disc, clipped outside by 0.05f) */
    ipo_gpu_cmd_set_viewport(&s_tp_gpu.batch, 0.0f, 0.0f, (float)ren_w, (float)gl_h, 0.0f, 1.0f);
    float model_mirr[16] = {
         cry,  0.0f, -sry, 0.0f,
         0.0f, -1.0f, 0.0f, 0.0f,
         sry,  0.0f,  cry, 0.0f,
         0.0f, -1.824f, 0.0f, 1.0f
    };
    float mvp_mirr[16];
    teapot_gpu_mat4_mul(mvp_mirr, vp, model_mirr);

    const pbr_material_t *mat = &materials[frame_mat];
    ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_PBR3D,
                             IPO_GPU_CULL_NONE, true, true, true, IPO_GPU_CMP_LESS, IPO_GPU_BLEND_NONE, false);
    teapot_gpu_transpose(mvp_mirr,   &vs_c[0]);
    teapot_gpu_transpose(model_mirr, &vs_c[16]);
    teapot_gpu_transpose(l_vp_tp,    &vs_c[32]);
    vs_c[48] = vs_c[16]; vs_c[49] = vs_c[17]; vs_c[50] = vs_c[18]; vs_c[51] = 0.0f;
    vs_c[52] = vs_c[20]; vs_c[53] = vs_c[21]; vs_c[54] = vs_c[22]; vs_c[55] = 0.0f;
    vs_c[56] = vs_c[24]; vs_c[57] = vs_c[25]; vs_c[58] = vs_c[26]; vs_c[59] = 0.0f;
    ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 0u, vs_c, 60u);

    fs_c[3]  = mat->env_strength * 0.48f;
    fs_c[4]  = mat->metallic; fs_c[5] = mat->roughness; fs_c[6] = mat->clearcoat * 0.55f;
    fs_c[7]  = 1.0f;
    fs_c[8]  = mat->albedo[0] * 0.45f; fs_c[9] = mat->albedo[1] * 0.45f; fs_c[10] = mat->albedo[2] * 0.45f;
    fs_c[15] = 0.0f;
    fs_c[16] = 0.20f; fs_c[17] = 0.22f; fs_c[18] = 0.28f; fs_c[19] = light1_enabled ? 4.0f : 1.0f;
    fs_c[20] = 0.06f; fs_c[21] = 0.06f; fs_c[22] = 0.07f;
    fs_c[24] = l0_dir[0]; fs_c[25] = l0_dir[1]; fs_c[26] = l0_dir[2]; fs_c[27] = 0.0f;
    fs_c[28] = 1.55f;     fs_c[29] = 1.45f;     fs_c[30] = 1.28f;     fs_c[31] = 0.0f;
    fs_c[32] = -0.72f; fs_c[33] = 0.48f; fs_c[34] = -0.50f; fs_c[35] = 0.0f;
    fs_c[36] = 0.48f;  fs_c[37] = 0.62f; fs_c[38] = 0.88f;  fs_c[39] = 0.0f;
    fs_c[40] = 0.68f;  fs_c[41] = 0.35f; fs_c[42] = -0.64f; fs_c[43] = 0.0f;
    fs_c[44] = 0.40f;  fs_c[45] = 0.42f; fs_c[46] = 0.48f;  fs_c[47] = 0.0f;
    fs_c[48] = -0.45f; fs_c[49] = -0.20f; fs_c[50] = 0.86f; fs_c[51] = 0.0f;
    fs_c[52] = 0.18f;  fs_c[53] = 0.17f;  fs_c[54] = 0.15f; fs_c[55] = 0.0f;
    ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, fs_c, 92u);
    ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.tp_vbo, s_tp_gpu.tp_ibo, 4u,
                     IPO_GPU_PRIM_TRIANGLES, 0u, s_tp_gpu.tp_tri_count * 3u, 0u, 0u);

    /* 2D: Reset ONLY depth to 1.0f before compositing translucent obsidian turntable & opaque foreground */
    float cz1[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ipo_gpu_cmd_clear(&s_tp_gpu.batch, IPO_GPU_CLEAR_DEPTH, cz1, 1.0f);

    /* 2E: Translucent Obsidian Top Disc (blends soft shadow + glossy reflection over mirror reflection) */
    ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_PBR3D,
                             IPO_GPU_CULL_NONE, true, true, true, IPO_GPU_CMP_LESS, IPO_GPU_BLEND_ALPHA, false);
    teapot_gpu_transpose(vp,       &vs_c[0]);
    teapot_gpu_transpose(model_id, &vs_c[16]);
    teapot_gpu_transpose(l_vp,     &vs_c[32]);
    memset(&vs_c[48], 0, 12u * sizeof(float));
    vs_c[48] = 1.0f; vs_c[53] = 1.0f; vs_c[58] = 1.0f;
    ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 0u, vs_c, 60u);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 0u, 0u, false, false);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 1u, shadows_enabled ? s_tp_gpu.shd_res : 0u, false, false);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 2u, s_tp_gpu.env_sharp_res, true, false);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 3u, s_tp_gpu.env_soft_res, true, false);

    fs_c[3] = 0.85f;
    fs_c[4] = 0.16f;  fs_c[5] = 0.08f;  fs_c[6] = 0.88f;  fs_c[7] = 1.0f;
    fs_c[8] = 0.048f; fs_c[9] = 0.052f; fs_c[10] = 0.064f; fs_c[11] = 0.0f;
    fs_c[15] = shadows_enabled ? 1.0f : 0.0f;
    fs_c[16] = 0.28f; fs_c[17] = 0.31f; fs_c[18] = 0.38f; fs_c[19] = light1_enabled ? 4.0f : 1.0f;
    fs_c[20] = 0.08f; fs_c[21] = 0.08f; fs_c[22] = 0.10f; fs_c[23] = 0.0012f;
    /* Light 0..3 full studio intensity */
    fs_c[24] = l0_dir[0]; fs_c[25] = l0_dir[1]; fs_c[26] = l0_dir[2]; fs_c[27] = 0.0f;
    fs_c[28] = 3.10f;     fs_c[29] = 2.90f;     fs_c[30] = 2.55f;     fs_c[31] = 0.60f;
    fs_c[32] = -0.72f; fs_c[33] = 0.48f; fs_c[34] = -0.50f; fs_c[35] = 0.0f;
    fs_c[36] = 0.95f;  fs_c[37] = 1.25f; fs_c[38] = 1.75f;  fs_c[39] = 0.0f;
    fs_c[40] = 0.68f;  fs_c[41] = 0.35f; fs_c[42] = -0.64f; fs_c[43] = 0.0f;
    fs_c[44] = 0.78f;  fs_c[45] = 0.84f; fs_c[46] = 0.96f;  fs_c[47] = 0.0f;
    fs_c[48] = -0.45f; fs_c[49] = -0.20f; fs_c[50] = 0.86f; fs_c[51] = 0.0f;
    fs_c[52] = 0.38f;  fs_c[53] = 0.34f;  fs_c[54] = 0.30f; fs_c[55] = 0.0f;
    fs_c[88] = have_env ? 1.0f : 0.0f;
    fs_c[89] = 1.35f / 1024.0f;
    fs_c[90] = 1.0f;
    fs_c[91] = 0.14f;
    ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, fs_c, 92u);
    ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.ped_vbo, 0u, 0u,
                     IPO_GPU_PRIM_TRIANGLES, 0u, ped_top_verts, 0u, 0u);

    /* 2F: Opaque Brushed Obsidian Bevel & Cylinder Wall */
    if (ped_side_verts > 0u) {
        ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_PBR3D,
                                 IPO_GPU_CULL_NONE, true, true, true, IPO_GPU_CMP_LESS, IPO_GPU_BLEND_NONE, false);
        fs_c[3]  = 0.62f; fs_c[5] = 0.24f; fs_c[6] = 0.55f; fs_c[31] = 0.0f;
        ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, fs_c, 92u);
        ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.ped_vbo, 0u, 0u,
                         IPO_GPU_PRIM_TRIANGLES, ped_top_verts, ped_side_verts, 0u, 0u);
    }

    /* Metallic champagne turntable inlay ring */
    if (s_tp_gpu.trim_vbo) {
        ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_PBR3D,
                                 IPO_GPU_CULL_NONE, true, true, true, IPO_GPU_CMP_LESS, IPO_GPU_BLEND_NONE, false);
        fs_c[3] = 1.32f; fs_c[4] = 0.96f; fs_c[5] = 0.14f; fs_c[6] = 0.55f; fs_c[7] = 1.0f;
        fs_c[8] = 0.88f; fs_c[9] = 0.76f; fs_c[10] = 0.54f;
        fs_c[31] = 0.0f;
        fs_c[90] = 0.0f;
        ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, fs_c, 92u);
        ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.trim_vbo, 0u, 0u,
                         IPO_GPU_PRIM_TRIANGLES, 0u, s_tp_gpu.trim_vert_cnt, 0u, 0u);
    }

    /* Subdivided PN-Triangle Teapot PBR draw (Model = RotateY(rot_y)) */
    ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_PBR3D,
                             IPO_GPU_CULL_NONE, true, true, true, IPO_GPU_CMP_LESS, IPO_GPU_BLEND_NONE, false);
    teapot_gpu_transpose(mvp_tp,   &vs_c[0]);
    teapot_gpu_transpose(model_tp, &vs_c[16]);
    teapot_gpu_transpose(l_vp_tp,  &vs_c[32]);
    vs_c[48] = vs_c[16]; vs_c[49] = vs_c[17]; vs_c[50] = vs_c[18]; vs_c[51] = 0.0f;
    vs_c[52] = vs_c[20]; vs_c[53] = vs_c[21]; vs_c[54] = vs_c[22]; vs_c[55] = 0.0f;
    vs_c[56] = vs_c[24]; vs_c[57] = vs_c[25]; vs_c[58] = vs_c[26]; vs_c[59] = 0.0f;
    ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 0u, vs_c, 60u);

    fs_c[3] = mat->env_strength * 1.08f;
    fs_c[4] = mat->metallic; fs_c[5] = mat->roughness; fs_c[6] = mat->clearcoat;
    fs_c[7] = 1.0f;
    fs_c[8] = mat->albedo[0]; fs_c[9] = mat->albedo[1]; fs_c[10] = mat->albedo[2];
    fs_c[16] = 0.34f; fs_c[17] = 0.38f; fs_c[18] = 0.46f;
    fs_c[20] = 0.12f; fs_c[21] = 0.11f; fs_c[22] = 0.11f;
    fs_c[31] = 0.0f;
    fs_c[90] = 0.0f;
    ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, fs_c, 92u);
    ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.tp_vbo, s_tp_gpu.tp_ibo, 4u,
                     IPO_GPU_PRIM_TRIANGLES, 0u, s_tp_gpu.tp_tri_count * 3u, 0u, 0u);

    /* MSAA Resolve Blit -> single-sample hdr_res */
    if (msaa_on) {
        ipo_gpu_cmd_blit(&s_tp_gpu.batch, s_tp_gpu.hdr_res, s_tp_gpu.hdr_msaa_res,
                         0, 0, ren_w, ren_h, 0, 0, ren_w, ren_h, false, false);
    }

    /* Pass 3: 1/4-Res HDR Optical Bloom Extract + Separable 9x9 Gaussian H/V Blur */
    bool have_bloom = (s_tp_gpu.bloom_a_res != 0u && s_tp_gpu.bloom_b_res != 0u);
    if (have_bloom) {
        float bw = (float)s_tp_gpu.bloom_w, bh = (float)s_tp_gpu.bloom_h;
        ipo_gpu_cmd_set_target(&s_tp_gpu.batch, s_tp_gpu.bloom_a_res, 0u,
                               (uint32_t)s_tp_gpu.bloom_w, (uint32_t)s_tp_gpu.bloom_h);
        ipo_gpu_cmd_set_viewport(&s_tp_gpu.batch, 0.0f, 0.0f, bw, bh, 0.0f, 1.0f);
        ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_BLOOM_EXTRACT,
                                 IPO_GPU_CULL_NONE, true, false, false, IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
        float be_c[4] = { 0.85f, 1.0f / 0.55f, 1.18f, 6.0f };
        ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, be_c, 4u);
        ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 0u, s_tp_gpu.hdr_res, true, false);
        ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.dyn2d_vbo.res_id, 0u, 0u,
                         IPO_GPU_PRIM_TRIANGLES, 6u, 6u, 0u, 0u);

        ipo_gpu_cmd_set_target(&s_tp_gpu.batch, s_tp_gpu.bloom_b_res, 0u,
                               (uint32_t)s_tp_gpu.bloom_w, (uint32_t)s_tp_gpu.bloom_h);
        ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_BLOOM_BLUR,
                                 IPO_GPU_CULL_NONE, true, false, false, IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
        float bh_c[4] = { 1.40f / bw, 0.0f, 0.0f, 0.0f };
        ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, bh_c, 4u);
        ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 0u, s_tp_gpu.bloom_a_res, true, false);
        ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.dyn2d_vbo.res_id, 0u, 0u,
                         IPO_GPU_PRIM_TRIANGLES, 6u, 6u, 0u, 0u);

        ipo_gpu_cmd_set_target(&s_tp_gpu.batch, s_tp_gpu.bloom_a_res, 0u,
                               (uint32_t)s_tp_gpu.bloom_w, (uint32_t)s_tp_gpu.bloom_h);
        float bv_c[4] = { 0.0f, 1.40f / bh, 0.0f, 0.0f };
        ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, bv_c, 4u);
        ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 0u, s_tp_gpu.bloom_b_res, true, false);
        ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.dyn2d_vbo.res_id, 0u, 0u,
                         IPO_GPU_PRIM_TRIANGLES, 6u, 6u, 0u, 0u);
    }

    /* Pass 4: Cine Post-FX (Multi-Tap Perceptual FXAA + Bloom + Vignette + ACES Tonemap + Gamma) -> sdr_res */
    ipo_gpu_cmd_set_target(&s_tp_gpu.batch, s_tp_gpu.sdr_res, 0u, (uint32_t)ren_w, (uint32_t)ren_h);
    ipo_gpu_cmd_set_viewport(&s_tp_gpu.batch, 0.0f, 0.0f, (float)ren_w, (float)ren_h, 0.0f, 1.0f);
    ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_TONEMAP,
                             IPO_GPU_CULL_NONE, true, false, false, IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
    float tm_c[8] = {
        1.05f, (float)IPO_TONEMAP_ACES, 1.0f, 0.0f,
        have_bloom ? 0.28f : 0.0f,
        1.0f / (float)ren_w, 1.0f / (float)ren_h,
        0.22f
    };
    ipo_gpu_cmd_set_constants(&s_tp_gpu.batch, 1u, tm_c, 8u);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 0u, s_tp_gpu.hdr_res, true, false);
    ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 1u, have_bloom ? s_tp_gpu.bloom_a_res : s_tp_gpu.hdr_res, true, false);
    ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.dyn2d_vbo.res_id, 0u, 0u,
                     IPO_GPU_PRIM_TRIANGLES, 6u, 6u, 0u, 0u);

    /* Pass 5: Crisp HUD Status Bar + Mouse Crosshair directly onto sdr_res */
    if (have_hud && s_tp_gpu.hud_tex_res) {
        ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_QUAD2D,
                                 IPO_GPU_CULL_NONE, true, false, false, IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
        ipo_gpu_cmd_bind_tex(&s_tp_gpu.batch, 0u, s_tp_gpu.hud_tex_res, false, false);
        ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.dyn2d_vbo.res_id, 0u, 0u,
                         IPO_GPU_PRIM_TRIANGLES, hud_v_start, 6u, 0u, 0u);
    }
    if (cur_v_cnt > 0u) {
        ipo_gpu_cmd_set_pipeline(&s_tp_gpu.batch, IPO_GPU_PROG_SOLID2D,
                                 IPO_GPU_CULL_NONE, true, false, false, IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
        ipo_gpu_cmd_draw(&s_tp_gpu.batch, s_tp_gpu.dyn2d_vbo.res_id, 0u, 0u,
                         IPO_GPU_PRIM_TRIANGLES, cur_v_start, cur_v_cnt, 0u, 0u);
    }

    if (win) {
        ipo_gpu_cmd_present_window(&s_tp_gpu.batch, s_tp_gpu.sdr_res, win, (uint32_t)ren_w, (uint32_t)ren_h);
    } else {
        ipo_gpu_cmd_present_screen(&s_tp_gpu.batch, s_tp_gpu.sdr_res, (uint32_t)ren_w, (uint32_t)ren_h);
    }
    uint32_t t_f0 = timer_millis();
    int f_rc = ipo_gpu_batch_flush(&s_tp_gpu.batch, s_tp_gpu.ctx_id);
    s_tp_gpu.last_flush_ms = timer_millis() - t_f0;
    return f_rc == 0;
}

static bool render_scene_gbuffer(int vp_w, int vp_h, int fb_h,
                                 float rot_y_deg,
                                 const float shadow_mat[16],
                                 bool want_hdr,
                                 int frame_mat) {
    init_teapot_data();
    ensure_screen_buffers(vp_w, vp_h, fb_h);
    ensure_backdrop_cache(vp_w, vp_h, want_hdr);
    if (!s_mesh_inited || !s_zbuf || !s_gbuf || !s_ped_zbuf || !s_ped_gbuf) return false;

    uint32_t sub_vc = s_sub_verts ? s_sub_vcount : (uint32_t)TEAPOT_NUM_VERTICES;
    uint32_t sub_tc = s_sub_idx   ? s_sub_tcount : (uint32_t)TEAPOT_NUM_TRIANGLES;
    if (!s_tverts) s_tverts = (gvert_t *)malloc((size_t)sub_vc * sizeof(gvert_t));
    if (!s_mverts) s_mverts = (gvert_t *)malloc((size_t)TEAPOT_NUM_VERTICES * sizeof(gvert_t));
    if (!s_sverts) s_sverts = (svert_t *)malloc((size_t)TEAPOT_NUM_VERTICES * sizeof(svert_t));
    if (!s_vis_tris) s_vis_tris = (vis_tri_t *)malloc((size_t)sub_tc * sizeof(vis_tri_t));
    if (!s_vis_shd_tris) s_vis_shd_tris = (vis_tri_t *)malloc((size_t)TEAPOT_NUM_TRIANGLES * sizeof(vis_tri_t));
    if (!s_tverts || !s_sverts || !s_vis_tris || !s_vis_shd_tris) return false;

    GLfloat mv_base[16], pj[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv_base);
    glGetFloatv(GL_PROJECTION_MATRIX, pj);
    float half_w = (float)vp_w * 0.5f;
    float half_h = (float)vp_h * 0.5f;

    /* 1B (Cached): Smooth 64-segment beveled turntable pedestal */
    bool need_ped_geom = (!s_ped_cache_valid ||
                          s_ped_cache_rotx != rot_x ||
                          s_ped_cache_cam  != cam_dist);
    bool need_ped_shade = (need_ped_geom ||
                           s_ped_cache_mat != frame_mat ||
                           s_ped_cache_shd != shadows_enabled ||
                           s_ped_cache_l1  != light1_enabled ||
                           s_ped_cache_hdr != want_hdr);
    if (need_ped_geom) {
        size_t need = (size_t)vp_w * (size_t)vp_h;
        memset(s_zbuf, 0xFF, need * sizeof(uint16_t));
        memset(s_gbuf, 0, need * sizeof(gbuf_px_t));
        s_cur_bx0 = vp_w - 1; s_cur_by0 = vp_h - 1;
        s_cur_bx1 = -1;       s_cur_by1 = -1;

        gvert_t v_center;
        gvert_t *v_mid = s_ped_mid, *r_top = s_ped_top, *r_bev = s_ped_bev, *r_bot = s_ped_bot;
        {
            float ex = mv_base[4] * (-0.912f) + mv_base[12];
            float ey = mv_base[5] * (-0.912f) + mv_base[13];
            float ez = mv_base[6] * (-0.912f) + mv_base[14];
            project_eye_to_screen(ex, ey, ez, pj, half_w, half_h, &v_center.sx, &v_center.sy, &v_center.sz);
            v_center.nx_fp = (int32_t)(mv_base[4] * 16383.0f * 1024.0f);
            v_center.ny_fp = (int32_t)(mv_base[5] * 16383.0f * 1024.0f);
            v_center.nz_fp = (int32_t)(mv_base[6] * 16383.0f * 1024.0f);
            v_center.ao_fp = 0;
        }

        const float r_foot = 1.05f;
        const float r_top_rad = 2.62f, y_top = -0.912f;
        const float r_bev_rad = 2.75f, y_bev = -0.962f;
        const float y_bot = -1.20f;

        for (int i = 0; i <= PED_SEGS; i++) {
            float ang = ((float)(i % PED_SEGS) / (float)PED_SEGS) * 6.2831853f;
            float ca = gl_cosf(ang), sa = gl_sinf(ang);
            {
                float ox = ca * r_foot, oz = sa * r_foot;
                float ex = mv_base[0] * ox + mv_base[4] * y_top + mv_base[8]  * oz + mv_base[12];
                float ey = mv_base[1] * ox + mv_base[5] * y_top + mv_base[9]  * oz + mv_base[13];
                float ez = mv_base[2] * ox + mv_base[6] * y_top + mv_base[10] * oz + mv_base[14];
                project_eye_to_screen(ex, ey, ez, pj, half_w, half_h, &v_mid[i].sx, &v_mid[i].sy, &v_mid[i].sz);
                v_mid[i].nx_fp = v_center.nx_fp;
                v_mid[i].ny_fp = v_center.ny_fp;
                v_mid[i].nz_fp = v_center.nz_fp;
                v_mid[i].ao_fp = (int32_t)(98.0f * 65536.0f);
            }
            {
                float ox = ca * r_top_rad, oz = sa * r_top_rad;
                float ex = mv_base[0] * ox + mv_base[4] * y_top + mv_base[8]  * oz + mv_base[12];
                float ey = mv_base[1] * ox + mv_base[5] * y_top + mv_base[9]  * oz + mv_base[13];
                float ez = mv_base[2] * ox + mv_base[6] * y_top + mv_base[10] * oz + mv_base[14];
                project_eye_to_screen(ex, ey, ez, pj, half_w, half_h, &r_top[i].sx, &r_top[i].sy, &r_top[i].sz);
                r_top[i].nx_fp = v_center.nx_fp;
                r_top[i].ny_fp = v_center.ny_fp;
                r_top[i].nz_fp = v_center.nz_fp;
                r_top[i].ao_fp = (int32_t)(250.0f * 65536.0f);
            }
            {
                float ox = ca * r_bev_rad, oz = sa * r_bev_rad;
                float ex = mv_base[0] * ox + mv_base[4] * y_bev + mv_base[8]  * oz + mv_base[12];
                float ey = mv_base[1] * ox + mv_base[5] * y_bev + mv_base[9]  * oz + mv_base[13];
                float ez = mv_base[2] * ox + mv_base[6] * y_bev + mv_base[10] * oz + mv_base[14];
                project_eye_to_screen(ex, ey, ez, pj, half_w, half_h, &r_bev[i].sx, &r_bev[i].sy, &r_bev[i].sz);
                float onx = ca * 0.70710678f, ony = 0.70710678f, onz = sa * 0.70710678f;
                r_bev[i].nx_fp = (int32_t)((mv_base[0] * onx + mv_base[4] * ony + mv_base[8]  * onz) * 16383.0f * 1024.0f);
                r_bev[i].ny_fp = (int32_t)((mv_base[1] * onx + mv_base[5] * ony + mv_base[9]  * onz) * 16383.0f * 1024.0f);
                r_bev[i].nz_fp = (int32_t)((mv_base[2] * onx + mv_base[6] * ony + mv_base[10] * onz) * 16383.0f * 1024.0f);
                r_bev[i].ao_fp = (int32_t)(255.0f * 65536.0f);
            }
            {
                float ox = ca * r_bev_rad, oz = sa * r_bev_rad;
                float ex = mv_base[0] * ox + mv_base[4] * y_bot + mv_base[8]  * oz + mv_base[12];
                float ey = mv_base[1] * ox + mv_base[5] * y_bot + mv_base[9]  * oz + mv_base[13];
                float ez = mv_base[2] * ox + mv_base[6] * y_bot + mv_base[10] * oz + mv_base[14];
                project_eye_to_screen(ex, ey, ez, pj, half_w, half_h, &r_bot[i].sx, &r_bot[i].sy, &r_bot[i].sz);
                r_bot[i].nx_fp = (int32_t)((mv_base[0] * ca + mv_base[8]  * sa) * 16383.0f * 1024.0f);
                r_bot[i].ny_fp = (int32_t)((mv_base[1] * ca + mv_base[9]  * sa) * 16383.0f * 1024.0f);
                r_bot[i].nz_fp = (int32_t)((mv_base[2] * ca + mv_base[10] * sa) * 16383.0f * 1024.0f);
                r_bot[i].ao_fp = (int32_t)(190.0f * 65536.0f);
            }
        }

        for (int i = 0; i < PED_SEGS; i++) {
            raster_gbuf_tri(vp_w, vp_h, &v_center, &v_mid[i], &v_mid[i + 1], 2);
            raster_gbuf_tri(vp_w, vp_h, &v_mid[i], &r_top[i], &v_mid[i + 1], 2);
            raster_gbuf_tri(vp_w, vp_h, &v_mid[i + 1], &r_top[i], &r_top[i + 1], 2);
            raster_gbuf_tri(vp_w, vp_h, &r_top[i], &r_bev[i], &r_top[i + 1], 4);
            raster_gbuf_tri(vp_w, vp_h, &r_top[i + 1], &r_bev[i], &r_bev[i + 1], 4);
            raster_gbuf_tri(vp_w, vp_h, &r_bev[i], &r_bot[i], &r_bev[i + 1], 3);
            raster_gbuf_tri(vp_w, vp_h, &r_bev[i + 1], &r_bot[i], &r_bot[i + 1], 3);
        }

        s_ped_bx0 = s_cur_bx0; s_ped_by0 = s_cur_by0;
        s_ped_bx1 = s_cur_bx1; s_ped_by1 = s_cur_by1;
        memcpy(s_ped_zbuf, s_zbuf, need * sizeof(uint16_t));
        memcpy(s_ped_gbuf, s_gbuf, need * sizeof(gbuf_px_t));
    } else {
        s_prev_bx0 = s_cur_bx0; s_prev_by0 = s_cur_by0;
        s_prev_bx1 = s_cur_bx1; s_prev_by1 = s_cur_by1;
        if (s_prev_bx1 >= s_prev_bx0 && s_prev_by1 >= s_prev_by0) {
            int cy0 = s_prev_by0 < 0 ? 0 : s_prev_by0;
            int cy1 = s_prev_by1 >= vp_h ? (vp_h - 1) : s_prev_by1;
            int cx0 = s_prev_bx0 < 0 ? 0 : s_prev_bx0;
            int cx1 = s_prev_bx1 >= vp_w ? (vp_w - 1) : s_prev_bx1;
            size_t span = (size_t)(cx1 - cx0 + 1);
            ped_restore_job_t prj = { .vp_w = vp_w, .cx0 = cx0, .cy0 = cy0, .cy1 = cy1, .span = span, .next_y = cy0 };
            tp_dispatch(ped_restore_worker, &prj);
        }
    }

    if (need_ped_shade) {
        size_t need = (size_t)vp_w * (size_t)vp_h;
        if (!need_ped_geom) {
            memcpy(s_gbuf, s_ped_gbuf, need * sizeof(gbuf_px_t));
        }
        if (want_hdr && s_studio_bg_hdr && s_bg_hdr_cache) {
            memcpy(s_bg_hdr_cache, s_studio_bg_hdr, need * sizeof(ipo_rgba64f_t));
            if (s_ped_bx1 >= s_ped_bx0 && s_ped_by1 >= s_ped_by0) {
                shade_pedestal_pass_64f(vp_w, s_ped_by0, s_ped_by1, s_ped_bx0, s_ped_bx1,
                                        s_bg_hdr_cache, frame_mat, false);
            }
        }
        s_ped_cache_valid = true;
        s_ped_cache_rotx  = rot_x;
        s_ped_cache_cam   = cam_dist;
        s_ped_cache_mat   = frame_mat;
        s_ped_cache_shd   = shadows_enabled;
        s_ped_cache_l1    = light1_enabled;
        s_ped_cache_hdr   = want_hdr;
        s_hdr_bg_primed   = false;
        s_prev_bx1 = -1; s_prev_by1 = -1;
    }

    s_cur_bx0 = vp_w - 1; s_cur_by0 = vp_h - 1;
    s_cur_bx1 = -1;       s_cur_by1 = -1;

    /* 1A. Half-resolution soft projected shadow mask + separable 5x5 Gaussian blur */
    int shd_w = (vp_w + 1) >> 1;
    int shd_h = (vp_h + 1) >> 1;
    s_shd_full_prev_x0 = s_shd_full_cur_x0;
    s_shd_full_prev_y0 = s_shd_full_cur_y0;
    s_shd_full_prev_x1 = s_shd_full_cur_x1;
    s_shd_full_prev_y1 = s_shd_full_cur_y1;
    s_shd_full_cur_x1  = -1;
    s_shd_full_cur_y1  = -1;

    if (s_shd_prev_x1 >= s_shd_prev_x0 && s_shd_prev_y1 >= s_shd_prev_y0 && s_shd_mask && s_shd_blur) {
        int sy0 = s_shd_prev_y0 < 0 ? 0 : s_shd_prev_y0;
        int sy1 = s_shd_prev_y1 >= shd_h ? (shd_h - 1) : s_shd_prev_y1;
        int sx0 = s_shd_prev_x0 < 0 ? 0 : s_shd_prev_x0;
        int sx1 = s_shd_prev_x1 >= shd_w ? (shd_w - 1) : s_shd_prev_x1;
        size_t span = (size_t)(sx1 - sx0 + 1);
        for (int y = sy0; y <= sy1; y++) {
            size_t off = (size_t)y * (size_t)shd_w + (size_t)sx0;
            memset(s_shd_mask + off, 0, span);
            memset(s_shd_blur + off, 0, span);
        }
    }

    float rad = rot_y_deg * (3.14159265f / 180.0f);
    float cos_r = gl_cosf(rad);
    float sin_r = gl_sinf(rad);

    bool do_shadow = (shadows_enabled && s_shd_mask && s_shd_tmp && s_shd_blur);
    float half_sw = (float)shd_w * 0.5f;
    float half_sh = (float)shd_h * 0.5f;
    float sm0 = 0.0f, sm4 = 0.0f, sm8 = 0.0f, sm12 = 0.0f;
    float sm1 = 0.0f, sm5 = 0.0f, sm9 = 0.0f, sm13 = 0.0f;
    float sm3 = 0.0f, sm7 = 0.0f, sm11 = 0.0f, sm15 = 0.0f;
    if (do_shadow) {
        /* Combine (proj_2d * mv_base * shadow_mat * R_y) once instead of per vertex */
        float pmv[16];
        for (int c = 0; c < 4; c++) {
            float mv0 = mv_base[c * 4 + 0];
            float mv1 = mv_base[c * 4 + 1];
            float mv2 = mv_base[c * 4 + 2];
            pmv[c * 4 + 0] = pj[0] * mv0 + pj[8] * mv2;
            pmv[c * 4 + 1] = pj[5] * mv1 + pj[9] * mv2;
            pmv[c * 4 + 2] = 0.0f;
            pmv[c * 4 + 3] = -mv2;
        }
        float pmv_sh[16];
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                pmv_sh[c * 4 + r] =
                    pmv[0 * 4 + r] * shadow_mat[c * 4 + 0] +
                    pmv[1 * 4 + r] * shadow_mat[c * 4 + 1] +
                    pmv[2 * 4 + r] * shadow_mat[c * 4 + 2] +
                    pmv[3 * 4 + r] * shadow_mat[c * 4 + 3];
            }
        }
        float ry_m[16] = {
            cos_r, 0.0f, -sin_r, 0.0f,
            0.0f,  1.0f,  0.0f,  0.0f,
            sin_r, 0.0f,  cos_r, 0.0f,
            0.0f,  0.0f,  0.0f,  1.0f
        };
        float sm[16];
        for (int c = 0; c < 4; c++) {
            for (int r = 0; r < 4; r++) {
                sm[c * 4 + r] =
                    pmv_sh[0 * 4 + r] * ry_m[c * 4 + 0] +
                    pmv_sh[1 * 4 + r] * ry_m[c * 4 + 1] +
                    pmv_sh[2 * 4 + r] * ry_m[c * 4 + 2] +
                    pmv_sh[3 * 4 + r] * ry_m[c * 4 + 3];
            }
        }
        sm0 = sm[0] * half_sw; sm4 = sm[4] * half_sw; sm8  = sm[8]  * half_sw; sm12 = sm[12] * half_sw;
        sm1 = sm[1] * half_sh; sm5 = sm[5] * half_sh; sm9  = sm[9]  * half_sh; sm13 = sm[13] * half_sh;
        sm3 = sm[3];           sm7 = sm[7];           sm11 = sm[11];           sm15 = sm[15];
    }

    /* 1C. Teapot mesh in rotated object space (orthonormal mv -> zero per-vertex sqrtf) */
    glPushMatrix();
    glRotatef(rot_y_deg, 0.0f, 1.0f, 0.0f);
    GLfloat mv[16];
    glGetFloatv(GL_MODELVIEW_MATRIX, mv);
    glPopMatrix();

    const float p0 = pj[0] * half_w;
    const float p5 = pj[5] * half_h;
    const float z_bias = 0.5f - pj[10] * 0.5f;
    const float z_scale = pj[14] * 0.5f;
    const float m0 = mv[0] * p0, m4 = mv[4] * p0, m8  = mv[8]  * p0, m12 = mv[12] * p0;
    const float m1 = mv[1] * p5, m5 = mv[5] * p5, m9  = mv[9]  * p5, m13 = mv[13] * p5;
    const float m2 = mv[2],      m6 = mv[6],      m10 = mv[10],      m14 = mv[14];

    const float nscale = 16383.0f * 1024.0f;
    const float nm0 = mv[0] * nscale, nm4 = mv[4] * nscale, nm8  = mv[8]  * nscale;
    const float nm1 = mv[1] * nscale, nm5 = mv[5] * nscale, nm9  = mv[9]  * nscale;
    const float nm2 = mv[2] * nscale, nm6 = mv[6] * nscale, nm10 = mv[10] * nscale;

    tvert_job_t tvj = {
        .half_w = half_w, .half_h = half_h,
        .z_bias = z_bias, .z_scale = z_scale,
        .m0 = m0, .m4 = m4, .m8 = m8, .m12 = m12,
        .m1 = m1, .m5 = m5, .m9 = m9, .m13 = m13,
        .m2 = m2, .m6 = m6, .m10 = m10, .m14 = m14,
        .nm0 = nm0, .nm4 = nm4, .nm8 = nm8,
        .nm1 = nm1, .nm5 = nm5, .nm9 = nm9,
        .nm2 = nm2, .nm6 = nm6, .nm10 = nm10,
        .sub_vcount = (int)sub_vc,
        .do_shadow = do_shadow,
        .do_mirror = (s_mverts != NULL && s_mirr_zbuf != NULL && s_mirr_gbuf != NULL && want_hdr),
        .half_sw = half_sw, .half_sh = half_sh,
        .sm0 = sm0, .sm4 = sm4, .sm8 = sm8, .sm12 = sm12,
        .sm1 = sm1, .sm5 = sm5, .sm9 = sm9, .sm13 = sm13,
        .sm3 = sm3, .sm7 = sm7, .sm11 = sm11, .sm15 = sm15,
        .next_v = 0
    };
    tp_dispatch(tvert_worker, &tvj);

    bool has_soft_shadow = false;
    if (do_shadow) {
        int bx0 = shd_w - 1, by0 = shd_h - 1, bx1 = 0, by1 = 0;
        int shd_vc = 0;
        for (int t = 0; t < TEAPOT_NUM_TRIANGLES; t++) {
            int idx = t * 3;
            const svert_t *sv0 = &s_sverts[teapot_indices[idx + 0]];
            const svert_t *sv1 = &s_sverts[teapot_indices[idx + 1]];
            const svert_t *sv2 = &s_sverts[teapot_indices[idx + 2]];
            float area = (sv1->sx - sv0->sx) * (sv2->sy - sv0->sy) -
                         (sv1->sy - sv0->sy) * (sv2->sx - sv0->sx);
            if (area >= 0.0f) continue;

            float fmin_y = sv0->sy < sv1->sy ? (sv0->sy < sv2->sy ? sv0->sy : sv2->sy) : (sv1->sy < sv2->sy ? sv1->sy : sv2->sy);
            float fmax_y = sv0->sy > sv1->sy ? (sv0->sy > sv2->sy ? sv0->sy : sv2->sy) : (sv1->sy > sv2->sy ? sv1->sy : sv2->sy);
            if (fmax_y - fmin_y < 0.3f) continue;

            float fmin_x = sv0->sx < sv1->sx ? (sv0->sx < sv2->sx ? sv0->sx : sv2->sx) : (sv1->sx < sv2->sx ? sv1->sx : sv2->sx);
            float fmax_x = sv0->sx > sv1->sx ? (sv0->sx > sv2->sx ? sv0->sx : sv2->sx) : (sv1->sx > sv2->sx ? sv1->sx : sv2->sx);
            if (fmax_x < 0.0f || fmin_x >= (float)shd_w) continue;

            int min_y = (int)(fmin_y + 0.5f);
            int max_y = (int)(fmax_y - 0.5f);
            if (min_y < 0) min_y = 0;
            if (max_y >= shd_h) max_y = shd_h - 1;
            if (min_y > max_y) continue;

            int min_x = (int)fmin_x;
            int max_x = (int)fmax_x;
            if (min_x < 0) min_x = 0;
            if (max_x >= shd_w) max_x = shd_w - 1;
            if (min_x > max_x) continue;

            if (min_x < bx0) bx0 = min_x;
            if (max_x > bx1) bx1 = max_x;
            if (min_y < by0) by0 = min_y;
            if (max_y > by1) by1 = max_y;

            s_vis_shd_tris[shd_vc].tri_idx = (uint16_t)t;
            s_vis_shd_tris[shd_vc].min_y   = (int16_t)min_y;
            s_vis_shd_tris[shd_vc].max_y   = (int16_t)max_y;
            shd_vc++;
        }

        if (shd_vc > 0 && bx1 >= bx0 && by1 >= by0) {
            raster_band_job_t srj;
            build_bands(&srj, by0, by1, shd_w, shd_h, shd_vc);
            if (srj.band_count > 0) tp_dispatch(shd_band_worker, &srj);
            free_bands(&srj);

            int pad = 4;
            int x0 = (bx0 - pad < 2) ? 2 : (bx0 - pad);
            int y0 = (by0 - pad < 2) ? 2 : (by0 - pad);
            int x1 = (bx1 + pad > shd_w - 3) ? (shd_w - 3) : (bx1 + pad);
            int y1 = (by1 + pad > shd_h - 3) ? (shd_h - 3) : (by1 + pad);

            int bxy0 = (y0 - 2 < 0) ? 0 : (y0 - 2);
            int bxy1 = (y1 + 2 >= shd_h) ? (shd_h - 1) : (y1 + 2);
            blur_job_t bxj = { .shd_w = shd_w, .shd_h = shd_h, .x0 = x0, .x1 = x1, .y_end = bxy1, .next_y = bxy0 };
            tp_dispatch(blur_x_worker, &bxj);

            blur_job_t byj = { .shd_w = shd_w, .shd_h = shd_h, .x0 = x0, .x1 = x1, .y_end = y1, .next_y = y0 };
            tp_dispatch(blur_y_worker, &byj);

            has_soft_shadow = true;
            s_shd_prev_x0 = (x0 - 2 < 0) ? 0 : (x0 - 2);
            s_shd_prev_y0 = (y0 - 2 < 0) ? 0 : (y0 - 2);
            s_shd_prev_x1 = (x1 + 2 >= shd_w) ? (shd_w - 1) : (x1 + 2);
            s_shd_prev_y1 = (y1 + 2 >= shd_h) ? (shd_h - 1) : (y1 + 2);
            s_shd_full_cur_x0 = s_shd_prev_x0 * 2;
            s_shd_full_cur_y0 = s_shd_prev_y0 * 2;
            s_shd_full_cur_x1 = s_shd_prev_x1 * 2 + 1;
            s_shd_full_cur_y1 = s_shd_prev_y1 * 2 + 1;
            if (s_shd_full_cur_x1 >= vp_w) s_shd_full_cur_x1 = vp_w - 1;
            if (s_shd_full_cur_y1 >= vp_h) s_shd_full_cur_y1 = vp_h - 1;
        } else {
            s_shd_prev_x1 = -1;
            s_shd_prev_y1 = -1;
        }
    } else {
        s_shd_prev_x1 = -1;
        s_shd_prev_y1 = -1;
    }

    int vis_count = 0;
    for (int t = 0; t < (int)sub_tc; t++) {
        uint32_t idx = (uint32_t)t * 3u;
        uint32_t i0 = s_sub_idx ? s_sub_idx[idx + 0u] : teapot_indices[idx + 0u];
        uint32_t i1 = s_sub_idx ? s_sub_idx[idx + 1u] : teapot_indices[idx + 1u];
        uint32_t i2 = s_sub_idx ? s_sub_idx[idx + 2u] : teapot_indices[idx + 2u];
        const gvert_t *v0 = &s_tverts[i0];
        const gvert_t *v1 = &s_tverts[i1];
        const gvert_t *v2 = &s_tverts[i2];

        float area = (v1->sx - v0->sx) * (v2->sy - v0->sy) -
                     (v1->sy - v0->sy) * (v2->sx - v0->sx);
        if (area >= 0.0f) continue;
        if (area > -0.04f) continue;

        float fmin_y = v0->sy < v1->sy ? (v0->sy < v2->sy ? v0->sy : v2->sy) : (v1->sy < v2->sy ? v1->sy : v2->sy);
        float fmax_y = v0->sy > v1->sy ? (v0->sy > v2->sy ? v0->sy : v2->sy) : (v1->sy > v2->sy ? v1->sy : v2->sy);
        if (fmax_y - fmin_y < 0.30f) continue;

        int min_y = (int)(fmin_y + 0.5f);
        int max_y = (int)(fmax_y - 0.5f);
        if (min_y < 0) min_y = 0;
        if (max_y >= vp_h) max_y = vp_h - 1;
        if (min_y > max_y) continue;

        float fmin_x = v0->sx < v1->sx ? (v0->sx < v2->sx ? v0->sx : v2->sx) : (v1->sx < v2->sx ? v1->sx : v2->sx);
        float fmax_x = v0->sx > v1->sx ? (v0->sx > v2->sx ? v0->sx : v2->sx) : (v1->sx > v2->sx ? v1->sx : v2->sx);
        int min_x = (int)fmin_x;
        int max_x = (int)fmax_x + 1;
        if (min_x < 0) min_x = 0;
        if (max_x >= vp_w) max_x = vp_w - 1;
        if (min_x > max_x) continue;

        if (min_x < s_cur_bx0) s_cur_bx0 = min_x;
        if (max_x > s_cur_bx1) s_cur_bx1 = max_x;
        if (min_y < s_cur_by0) s_cur_by0 = min_y;
        if (max_y > s_cur_by1) s_cur_by1 = max_y;

        s_vis_tris[vis_count].tri_idx = (uint16_t)t;
        s_vis_tris[vis_count].min_y   = (int16_t)min_y;
        s_vis_tris[vis_count].max_y   = (int16_t)max_y;
        vis_count++;
    }

    if (vis_count > 0 && s_cur_by1 >= s_cur_by0) {
        raster_band_job_t rj;
        build_bands(&rj, s_cur_by0, s_cur_by1, vp_w, vp_h, vis_count);
        if (rj.band_count > 0) tp_dispatch(raster_band_worker, &rj);
        free_bands(&rj);
    }

    /* 1D: Mirror reflection pass onto pedestal top (mat == 2) */
    if (s_mirr_prev_x1 >= s_mirr_prev_x0 && s_mirr_prev_y1 >= s_mirr_prev_y0 && s_mirr_zbuf && s_mirr_gbuf) {
        int my0 = (s_mirr_prev_y0 < 0) ? 0 : s_mirr_prev_y0;
        int my1 = (s_mirr_prev_y1 >= vp_h) ? (vp_h - 1) : s_mirr_prev_y1;
        int mx0 = (s_mirr_prev_x0 < 0) ? 0 : s_mirr_prev_x0;
        int mx1 = (s_mirr_prev_x1 >= vp_w) ? (vp_w - 1) : s_mirr_prev_x1;
        size_t mspan = (size_t)(mx1 - mx0 + 1);
        for (int y = my0; y <= my1; y++) {
            size_t off = (size_t)y * (size_t)vp_w + (size_t)mx0;
            memset(s_mirr_zbuf + off, 0xFF, mspan * sizeof(uint16_t));
            memset(s_mirr_gbuf + off, 0, mspan * sizeof(mirr_px_t));
        }
    }
    s_mirr_bx0 = vp_w - 1; s_mirr_by0 = vp_h - 1;
    s_mirr_bx1 = -1;       s_mirr_by1 = -1;
    if (tvj.do_mirror && s_ped_bx1 >= s_ped_bx0 && s_ped_by1 >= s_ped_by0) {
        int px0 = (s_ped_bx0 < 0) ? 0 : s_ped_bx0;
        int py0 = (s_ped_by0 < 0) ? 0 : s_ped_by0;
        int px1 = (s_ped_bx1 >= vp_w) ? (vp_w - 1) : s_ped_bx1;
        int py1 = (s_ped_by1 >= vp_h) ? (vp_h - 1) : s_ped_by1;
        int mvc = 0;
        for (int t = 0; t < TEAPOT_NUM_TRIANGLES; t++) {
            int idx = t * 3;
            const gvert_t *v0 = &s_mverts[teapot_indices[idx + 0]];
            const gvert_t *v1 = &s_mverts[teapot_indices[idx + 1]];
            const gvert_t *v2 = &s_mverts[teapot_indices[idx + 2]];
            float area = (v1->sx - v0->sx) * (v2->sy - v0->sy) - (v1->sy - v0->sy) * (v2->sx - v0->sx);
            if (area <= 0.05f) continue;
            float fmin_y = v0->sy < v1->sy ? (v0->sy < v2->sy ? v0->sy : v2->sy) : (v1->sy < v2->sy ? v1->sy : v2->sy);
            float fmax_y = v0->sy > v1->sy ? (v0->sy > v2->sy ? v0->sy : v2->sy) : (v1->sy > v2->sy ? v1->sy : v2->sy);
            int my0 = (int)fmin_y;
            int my1 = (int)fmax_y + 1;
            if (my1 < py0 || my0 > py1) continue;
            s_vis_shd_tris[mvc].tri_idx = (uint16_t)t;
            s_vis_shd_tris[mvc].min_y   = (int16_t)my0;
            s_vis_shd_tris[mvc].max_y   = (int16_t)my1;
            mvc++;
        }
        if (mvc > 0 && py1 >= py0) {
            mirror_band_job_t mj;
            mj.vp_w = vp_w; mj.px0 = px0; mj.px1 = px1; mj.mvc = mvc; mj.next_band = 0;
            int nb = (s_pool.worker_count + 1) * 2;
            int span = py1 - py0 + 1;
            if (nb > span) nb = span;
            if (nb > 16) nb = 16;
            if (nb < 1) nb = 1;
            mj.band_count = nb;
            int y = py0;
            for (int b = 0; b < nb; b++) {
                int ny = py0 + ((span * (b + 1)) / nb);
                mj.band_y0[b] = y;
                mj.band_y1[b] = (b == nb - 1) ? py1 : (ny - 1);
                y = ny;
            }
            tp_dispatch(mirror_band_worker, &mj);
            for (int b = 0; b < nb; b++) {
                if (mj.l_bx1[b] >= mj.l_bx0[b] && mj.l_by1[b] >= mj.l_by0[b]) {
                    if (mj.l_bx0[b] < s_mirr_bx0) s_mirr_bx0 = mj.l_bx0[b];
                    if (mj.l_bx1[b] > s_mirr_bx1) s_mirr_bx1 = mj.l_bx1[b];
                    if (mj.l_by0[b] < s_mirr_by0) s_mirr_by0 = mj.l_by0[b];
                    if (mj.l_by1[b] > s_mirr_by1) s_mirr_by1 = mj.l_by1[b];
                }
            }
        }
        if (s_mirr_bx1 >= s_mirr_bx0 && s_mirr_by1 >= s_mirr_by0) {
            if (s_mirr_bx0 < s_cur_bx0) s_cur_bx0 = s_mirr_bx0;
            if (s_mirr_bx1 > s_cur_bx1) s_cur_bx1 = s_mirr_bx1;
            if (s_mirr_by0 < s_cur_by0) s_cur_by0 = s_mirr_by0;
            if (s_mirr_by1 > s_cur_by1) s_cur_by1 = s_mirr_by1;
        }
    }
    s_mirr_prev_x0 = s_mirr_bx0; s_mirr_prev_y0 = s_mirr_by0;
    s_mirr_prev_x1 = s_mirr_bx1; s_mirr_prev_y1 = s_mirr_by1;

    return has_soft_shadow;
}

/* =========================================================================
 * Pass 2: Deferred Fixed-Point PBR + HDR Cubemap -> WM 8bpp Framebuffer
 * ========================================================================= */
static void shade_deferred_pbr_to_wm8(int w, int h,
                                      uint8_t *dst8, int dst_stride,
                                      int mat_idx,
                                      bool has_soft_shadow) {
    if (!dst8 || !s_gbuf || !s_bg_idx_cache || !s_vx_col || !s_vy_row) return;

    const pbr_material_t *mat = &materials[mat_idx];
    int alb_r = (int)(mat->albedo[0] * 256.0f + 0.5f);
    int alb_g = (int)(mat->albedo[1] * 256.0f + 0.5f);
    int alb_b = (int)(mat->albedo[2] * 256.0f + 0.5f);
    int f0_r  = (int)(mat->f0[0] * 256.0f + 0.5f);
    int f0_g  = (int)(mat->f0[1] * 256.0f + 0.5f);
    int f0_b  = (int)(mat->f0[2] * 256.0f + 0.5f);
    int metal = (int)(mat->metallic * 256.0f + 0.5f);
    int diff_w = 256 - ((metal * 235) >> 8);
    int env_s  = (int)(mat->env_strength * 256.0f + 0.5f);

    int spec_tint_r = 256 - metal + ((metal * alb_r) >> 8);
    int spec_tint_g = 256 - metal + ((metal * alb_g) >> 8);
    int spec_tint_b = 256 - metal + ((metal * alb_b) >> 8);

    const uint16_t *spec_lut = s_spec_ggx_lut[mat_idx];

    /* Q8 unit light & half vectors (key warm light + cool rim fill light) */
    const int l0x = 133, l0y = 184, l0z = 118;
    const int h0x = 78,  h0y = 108, h0z = 219;
    const int l1x = -174, l1y = 133, l1z = -133;
    const int h1x = -141, h1y = 108, h1z = 184;
    const bool shd_on = shadows_enabled;
    const bool l1_on  = light1_enabled;
    const int shd_w = (w + 1) >> 1;

    for (int y = 0; y < h; y++) {
        const gbuf_px_t *grow = &s_gbuf[(size_t)y * (size_t)w];
        const uint8_t *bgrow  = &s_bg_idx_cache[(size_t)y * (size_t)w];
        const uint8_t *shdrow = (has_soft_shadow && s_shd_blur) ? &s_shd_blur[(size_t)(y >> 1) * (size_t)shd_w] : NULL;
        uint8_t *drow = dst8 + (size_t)y * (size_t)dst_stride;
        const int8_t *irow = s_ign_tile[y & 63];
        int vy_raw = (int)s_vy_row[y];

        for (int x = 0; x < w; x++) {
            uint8_t mat_id = grow[x].mat;
            if (mat_id == 0u) {
                drow[x] = bgrow[x];
                continue;
            }

            int nx_i = (int)grow[x].nx >> 7;
            int ny_i = (int)grow[x].ny >> 7;
            int nz_i = (int)grow[x].nz >> 7;
            int len2 = nx_i * nx_i + ny_i * ny_i + nz_i * nz_i;
            int slot = len2 >> 4;
            if (slot >= INV_LEN_SLOTS) slot = INV_LEN_SLOTS - 1;
            uint32_t inv_l = s_inv_len_q16[slot];

            int Nx = (int)((int32_t)nx_i * (int32_t)inv_l >> 16);
            int Ny = (int)((int32_t)ny_i * (int32_t)inv_l >> 16);
            int Nz = (int)((int32_t)nz_i * (int32_t)inv_l >> 16);

            int bias = (int)irow[x & 63];

            if (mat_id != 1u) {
                /* Studio turntable plinth (top, bevel catchlight, skirt) */
                int ndotl0 = (Nx * l0x + Ny * l0y + Nz * l0z) >> 8;
                if (ndotl0 < 0) ndotl0 = 0; else if (ndotl0 > 256) ndotl0 = 256;

                int ndotl1 = 0;
                if (l1_on) {
                    ndotl1 = (Nx * l1x + Ny * l1y + Nz * l1z) >> 8;
                    if (ndotl1 < 0) ndotl1 = 0; else if (ndotl1 > 256) ndotl1 = 256;
                }

                int nh = (Nx * h0x + Ny * h0y + Nz * h0z) >> 8;
                if (nh < 0) nh = 0; else if (nh > 256) nh = 256;
                int spec = (int)s_ped_spec_lut[nh];

                int rf, gf, bf;
                if (mat_id == 2u) {
                    int r_param = (int)grow[x].ao; /* 0 (center) .. 250 (rim) */

                    /* Contact AO curve directly under teapot base foot ring (~r_param=74..115) */
                    int contact_ao = 256;
                    if (shd_on) {
                        int d_ring = r_param - 76;
                        if (d_ring < 0) d_ring = (-d_ring * 3) >> 2;
                        if (d_ring < 82) {
                            contact_ao = 58 + (d_ring * 198) / 82;
                        }
                    }

                    int proj_vis = shdrow ? (256 - (((int)shdrow[x >> 1] * 188) >> 8)) : 256;
                    int vis = (contact_ao * proj_vis) >> 8;

                    /* Overhead spotlight radial falloff + subtle brushed turntable ring */
                    int spot = 256 - ((r_param * r_param) >> 8);
                    if (spot < 74) spot = 74;
                    int lathe = (((r_param >> 3) & 1) << 1);
                    int base = ((36 + lathe) * spot) >> 8;

                    /* Subtle Fresnel sheen catchlight + soft studio plinth bounce */
                    int bounce_r = 0, bounce_g = 0, bounce_b = 0;
                    if (r_param > 72 && r_param < 168) {
                        int bleed = (168 - r_param) >> 4;
                        bounce_r = (alb_r * bleed) >> 8;
                        bounce_g = (alb_g * bleed) >> 8;
                        bounce_b = (alb_b * bleed) >> 8;
                    }

                    int key_ill = (ndotl0 * vis) >> 8;
                    int s_vis = (spec * vis) >> 8;
                    rf = ((base * (62 + ((196 * key_ill + 46 * ndotl1) >> 8))) >> 8) + s_vis + bounce_r;
                    gf = ((base * (66 + ((190 * key_ill + 58 * ndotl1) >> 8))) >> 8) + s_vis + bounce_g;
                    bf = ((base * (74 + ((180 * key_ill + 76 * ndotl1) >> 8))) >> 8) + s_vis + bounce_b;
                } else {
                    /* Crisp metallic bevel highlight & brushed obsidian skirt */
                    int base = (mat_id == 4u) ? 44 : 30;
                    int sp_mult = (mat_id == 4u) ? (spec * 3) >> 1 : spec;
                    rf = ((base * (84 + ((218 * ndotl0 + 52 * ndotl1) >> 8))) >> 8) + sp_mult;
                    gf = ((base * (88 + ((212 * ndotl0 + 66 * ndotl1) >> 8))) >> 8) + sp_mult;
                    bf = ((base * (98 + ((204 * ndotl0 + 88 * ndotl1) >> 8))) >> 8) + sp_mult;
                }

                if (rf < 0) rf = 0; else if (rf > 256) rf = 256;
                if (gf < 0) gf = 0; else if (gf > 256) gf = 256;
                if (bf < 0) bf = 0; else if (bf > 256) bf = 256;

                int r8 = (int)s_srgb_lut[rf];
                int g8 = (int)s_srgb_lut[gf];
                int b8 = (int)s_srgb_lut[bf];
                /* Low-amplitude dither avoids background/pedestal static */
                drow[x] = rgb_dither_to_wm8(r8, g8, b8, bias >> 1);
                continue;
            }

            /* === Photorealistic Teapot PBR Shader (mat_id == 1) === */
            int vx = (int)s_vx_col[x];
            int vy = vy_raw;
            int vz = 248;

            int ndotv = (Nx * vx + Ny * vy + Nz * vz) >> 8;
            if (ndotv < 0) ndotv = 0; else if (ndotv > 256) ndotv = 256;

            /* Exact Schlick Fresnel F = F0 + (1 - F0)*(1 - N.V)^5 */
            int one_m = 256 - ndotv;
            int f2 = (one_m * one_m) >> 8;
            int f4 = (f2 * f2) >> 8;
            int f5 = (f4 * one_m) >> 8;
            int fr = f0_r + (((256 - f0_r) * f5) >> 8);
            int fg = f0_g + (((256 - f0_g) * f5) >> 8);
            int fb = f0_b + (((256 - f0_b) * f5) >> 8);

            /* Two studio lights (key warm + cool rim fill) with GGX NDF LUT */
            int ndotl0 = (Nx * l0x + Ny * l0y + Nz * l0z) >> 8;
            int spec0 = 0;
            if (ndotl0 > 0) {
                if (ndotl0 > 256) ndotl0 = 256;
                int nh0 = (Nx * h0x + Ny * h0y + Nz * h0z) >> 8;
                if (nh0 > 0) {
                    if (nh0 > 256) nh0 = 256;
                    spec0 = (int)spec_lut[nh0];
                }
            } else {
                ndotl0 = 0;
            }

            int ndotl1 = 0, spec1 = 0;
            if (l1_on) {
                ndotl1 = (Nx * l1x + Ny * l1y + Nz * l1z) >> 8;
                if (ndotl1 > 0) {
                    if (ndotl1 > 256) ndotl1 = 256;
                    int nh1 = (Nx * h1x + Ny * h1y + Nz * h1z) >> 8;
                    if (nh1 > 0) {
                        if (nh1 > 256) nh1 = 256;
                        spec1 = ((int)spec_lut[nh1]) >> 1;
                    }
                } else {
                    ndotl1 = 0;
                }
            }

            /* 3D Cavity Ambient Occlusion + Hemisphere Sky / Pedestal Bounce */
            int ao = (int)grow[x].ao + 1; /* 1..256 */
            int hemi = 128 + (Ny >> 1);
            int amb_r = ((32 + ((34 * hemi) >> 8)) * ao) >> 8;
            int amb_g = ((30 + ((38 * hemi) >> 8)) * ao) >> 8;
            int amb_b = ((30 + ((48 * hemi) >> 8)) * ao) >> 8;

            /* Subtle diffuse shadow softening inside AO pockets */
            int ao_vis = (128 + (ao >> 1));
            int irr_r = amb_r + (((296 * ndotl0 + 64 * ndotl1) >> 8) * ao_vis >> 8);
            int irr_g = amb_g + (((280 * ndotl0 + 88 * ndotl1) >> 8) * ao_vis >> 8);
            int irr_b = amb_b + (((252 * ndotl0 + 132 * ndotl1) >> 8) * ao_vis >> 8);

            /* Seamless 6-face Studio Softbox HDRI Reflection (R = 2(N.V)N - V) */
            int two_nv = ndotv << 1;
            int rx = ((two_nv * Nx) >> 8) - vx;
            int ry = ((two_nv * Ny) >> 8) - vy;
            int rz = ((two_nv * Nz) >> 8) - vz;
            int env[3];
            sample_cubemap_q8(rx, ry, rz, env);

            int env_ao_s = (env_s * ao) >> 8;
            int refl_r = (env[0] * env_ao_s) >> 8;
            int refl_g = (env[1] * env_ao_s) >> 8;
            int refl_b = (env[2] * env_ao_s) >> 8;

            int kd_r = (((256 - fr) * diff_w) >> 8) * alb_r >> 8;
            int kd_g = (((256 - fg) * diff_w) >> 8) * alb_g >> 8;
            int kd_b = (((256 - fb) * diff_w) >> 8) * alb_b >> 8;

            int sp_r = ((spec0 * 410 + spec1 * 120) >> 8) * spec_tint_r >> 8;
            int sp_g = ((spec0 * 384 + spec1 * 156) >> 8) * spec_tint_g >> 8;
            int sp_b = ((spec0 * 348 + spec1 * 218) >> 8) * spec_tint_b >> 8;

            int hdr_r = ((kd_r * irr_r) >> 8) + ((fr * refl_r) >> 8) + sp_r;
            int hdr_g = ((kd_g * irr_g) >> 8) + ((fg * refl_g) >> 8) + sp_g;
            int hdr_b = ((kd_b * irr_b) >> 8) + ((fb * refl_b) >> 8) + sp_b;

            if (hdr_r < 0) hdr_r = 0; else if (hdr_r >= HDR_Q8_MAX) hdr_r = HDR_Q8_MAX - 1;
            if (hdr_g < 0) hdr_g = 0; else if (hdr_g >= HDR_Q8_MAX) hdr_g = HDR_Q8_MAX - 1;
            if (hdr_b < 0) hdr_b = 0; else if (hdr_b >= HDR_Q8_MAX) hdr_b = HDR_Q8_MAX - 1;

            int out_r = (int)s_aces_lut[hdr_r];
            int out_g = (int)s_aces_lut[hdr_g];
            int out_b = (int)s_aces_lut[hdr_b];

            drow[x] = rgb_dither_to_wm8(out_r, out_g, out_b, bias);
        }
    }
}

/* =========================================================================
 * Pass 2 (TrueColor / Priority 256-bpp RGBA64F): Photorealistic Linear HDR PBR
 * ========================================================================= */
static inline void sample_cubemap_q12(int32_t rx, int32_t ry, int32_t rz, int32_t out[3]) {
    int32_t ax = (rx >= 0) ? rx : -rx;
    int32_t ay = (ry >= 0) ? ry : -ry;
    int32_t az = (rz >= 0) ? rz : -rz;

    int face;
    int32_t uc, vc, m;
    if (ax >= ay && ax >= az) {
        m = ax;
        if (rx >= 0) { face = 0; uc = -rz; vc = -ry; }
        else         { face = 1; uc =  rz; vc = -ry; }
    } else if (ay >= az) {
        m = ay;
        if (ry >= 0) { face = 2; uc =  rx; vc =  rz; }
        else         { face = 3; uc =  rx; vc = -rz; }
    } else {
        m = az;
        if (rz >= 0) { face = 4; uc =  rx; vc = -ry; }
        else         { face = 5; uc = -rx; vc = -ry; }
    }
    if (m < 8) m = 8;
    else if (m > 16384) m = 16384;

    int32_t sc = (int32_t)s_cube_scale_q12[m >> 3];
    int32_t u = ((uc * sc) >> 12) + ((CUBE_SZ - 1) * 128);
    int32_t v = ((vc * sc) >> 12) + ((CUBE_SZ - 1) * 128);
    if (u < 0) u = 0; else if (u > CUBE_UV_MAX) u = CUBE_UV_MAX;
    if (v < 0) v = 0; else if (v > CUBE_UV_MAX) v = CUBE_UV_MAX;

    int x0 = u >> 8;
    int y0 = v >> 8;
    int fx = u & 255;
    int fy = v & 255;

    const int16_t *p00 = s_env_cube_q12[face][y0][x0];
    const int16_t *p10 = s_env_cube_q12[face][y0][x0 + 1];
    const int16_t *p01 = s_env_cube_q12[face][y0 + 1][x0];
    const int16_t *p11 = s_env_cube_q12[face][y0 + 1][x0 + 1];

    int32_t t0 = ((int32_t)p00[0] << 8) + fx * ((int32_t)p10[0] - (int32_t)p00[0]);
    int32_t b0 = ((int32_t)p01[0] << 8) + fx * ((int32_t)p11[0] - (int32_t)p01[0]);
    int32_t t1 = ((int32_t)p00[1] << 8) + fx * ((int32_t)p10[1] - (int32_t)p00[1]);
    int32_t b1 = ((int32_t)p01[1] << 8) + fx * ((int32_t)p11[1] - (int32_t)p01[1]);
    int32_t t2 = ((int32_t)p00[2] << 8) + fx * ((int32_t)p10[2] - (int32_t)p00[2]);
    int32_t b2 = ((int32_t)p01[2] << 8) + fx * ((int32_t)p11[2] - (int32_t)p01[2]);
    out[0] = ((t0 << 8) + fy * (b0 - t0)) >> 16;
    out[1] = ((t1 << 8) + fy * (b1 - t1)) >> 16;
    out[2] = ((t2 << 8) + fy * (b2 - t2)) >> 16;
}

static __attribute__((noinline)) void shade_pedestal_pass_64f(
        int w, int sy0, int sy1, int sx0, int sx1,
        ipo_rgba64f_t *dst64, int mat_idx, bool has_soft_shadow) {
    const pbr_material_t *mat = &materials[mat_idx];
    const float alb_r = mat->albedo[0];
    const float alb_g = mat->albedo[1];
    const float alb_b = mat->albedo[2];
    const bool shd_on = shadows_enabled;
    const bool l1_on  = light1_enabled;
    const int shd_w   = (w + 1) >> 1;

    int16_t ped_last_nx = 32767, ped_last_ny = 32767, ped_last_nz = 32767;
    float ped_ndotl0 = 0.0f, ped_ndotl1 = 0.0f, ped_spec = 0.0f;
    ipo_rgba64f_t ped_hdr_mat3 = { 0.0, 0.0, 0.0, 1.0 };
    ipo_rgba64f_t ped_hdr_mat4 = { 0.0, 0.0, 0.0, 1.0 };

    static ipo_rgba64f_t s_ped_top_lut[256];
    static int16_t s_ped_lut_nx = 32767, s_ped_lut_ny = 32767, s_ped_lut_nz = 32767;
    static int s_ped_lut_mat = -1;
    static bool s_ped_lut_shd = false, s_ped_lut_l1 = false;
    bool ped_lut_ready = (s_ped_lut_mat == mat_idx && s_ped_lut_shd == shd_on && s_ped_lut_l1 == l1_on);

    for (int y = sy0; y <= sy1; y++) {
        size_t row_off = (size_t)y * (size_t)w;
        const gbuf_px_t *grow = &s_gbuf[row_off];
        const uint8_t *shdrow = (has_soft_shadow && s_shd_blur) ? &s_shd_blur[(size_t)(y >> 1) * (size_t)shd_w] : NULL;
        ipo_rgba64f_t *drow = &dst64[row_off];

        for (int x = sx0; x <= sx1; x++) {
            uint8_t mat_id = grow[x].mat;
            if (mat_id <= 1u) continue;

            if (grow[x].nx != ped_last_nx || grow[x].ny != ped_last_ny || grow[x].nz != ped_last_nz) {
                ped_last_nx = grow[x].nx;
                ped_last_ny = grow[x].ny;
                ped_last_nz = grow[x].nz;
                int32_t pnx, pny, pnz;
                norm_vec3_q14((int32_t)ped_last_nx, (int32_t)ped_last_ny, (int32_t)ped_last_nz, &pnx, &pny, &pnz);
                float Nx = (float)pnx * (1.0f / 16384.0f);
                float Ny = (float)pny * (1.0f / 16384.0f);
                float Nz = (float)pnz * (1.0f / 16384.0f);

                ped_ndotl0 = Nx * 0.5195f + Ny * 0.7188f + Nz * 0.4609f;
                if (ped_ndotl0 < 0.0f) ped_ndotl0 = 0.0f; else if (ped_ndotl0 > 1.0f) ped_ndotl0 = 1.0f;

                ped_ndotl1 = 0.0f;
                if (l1_on) {
                    ped_ndotl1 = Nx * (-0.6797f) + Ny * 0.5195f + Nz * (-0.5195f);
                    if (ped_ndotl1 < 0.0f) ped_ndotl1 = 0.0f; else if (ped_ndotl1 > 1.0f) ped_ndotl1 = 1.0f;
                }

                float nh = Nx * 0.3047f + Ny * 0.4219f + Nz * 0.8555f;
                ped_spec = 0.0f;
                if (nh > 0.35f) {
                    const float pa2 = 0.14f * 0.14f;
                    float denom = nh * nh * (pa2 - 1.0f) + 1.0f;
                    ped_spec = (pa2 * 0.31830988618f / (denom * denom + 1e-6f)) * 0.016f;
                    if (ped_spec > 1.0f) ped_spec = 1.0f;
                }

                float c0 = 0.328f + 0.852f * ped_ndotl0 + 0.203f * ped_ndotl1;
                float c1 = 0.344f + 0.828f * ped_ndotl0 + 0.258f * ped_ndotl1;
                float c2 = 0.383f + 0.797f * ped_ndotl0 + 0.344f * ped_ndotl1;
                ped_hdr_mat3.r = (double)((30.0f / 256.0f) * c0 + ped_spec);
                ped_hdr_mat3.g = (double)((30.0f / 256.0f) * c1 + ped_spec);
                ped_hdr_mat3.b = (double)((30.0f / 256.0f) * c2 + ped_spec);
                ped_hdr_mat4.r = (double)((44.0f / 256.0f) * c0 + ped_spec * 1.5f);
                ped_hdr_mat4.g = (double)((44.0f / 256.0f) * c1 + ped_spec * 1.5f);
                ped_hdr_mat4.b = (double)((44.0f / 256.0f) * c2 + ped_spec * 1.5f);
            }
            if (mat_id == 2u) {
                if (!ped_lut_ready || s_ped_lut_nx != ped_last_nx ||
                    s_ped_lut_ny != ped_last_ny || s_ped_lut_nz != ped_last_nz) {
                    for (int aoi = 0; aoi < 256; aoi++) {
                        float r_param = (float)aoi * (1.0f / 256.0f);
                        float contact_ao = 1.0f;
                        if (shd_on) {
                            float d_ring = r_param - (76.0f / 256.0f);
                            if (d_ring < 0.0f) d_ring = -d_ring * 0.75f;
                            if (d_ring < (82.0f / 256.0f)) {
                                contact_ao = (58.0f / 256.0f) + (d_ring * (198.0f / 82.0f));
                            }
                        }
                        float spot = 1.0f - r_param * r_param;
                        if (spot < 0.289f) spot = 0.289f;
                        float base = (38.0f / 256.0f) * spot;
                        float br = 0.0f, bg = 0.0f, bb = 0.0f;
                        if (aoi > 72 && aoi < 168) {
                            float bleed = (float)(168 - aoi) * (1.0f / 4096.0f);
                            br = alb_r * bleed; bg = alb_g * bleed; bb = alb_b * bleed;
                        }
                        float ki = ped_ndotl0 * contact_ao;
                        float sv = ped_spec * contact_ao;
                        double ar = (double)(base * (0.242f + 0.180f * ped_ndotl1) + br);
                        double ag = (double)(base * (0.258f + 0.226f * ped_ndotl1) + bg);
                        double ab = (double)(base * (0.289f + 0.297f * ped_ndotl1) + bb);
                        double dr = (double)(base * (0.765f * ki) + sv);
                        double dg = (double)(base * (0.742f * ki) + sv);
                        double db = (double)(base * (0.703f * ki) + sv);
                        int ia_r = (int)(ar * 4096.0 + 0.5); if (ia_r < 0) ia_r = 0; else if (ia_r > 65535) ia_r = 65535;
                        int ia_g = (int)(ag * 4096.0 + 0.5); if (ia_g < 0) ia_g = 0; else if (ia_g > 65535) ia_g = 65535;
                        int ia_b = (int)(ab * 4096.0 + 0.5); if (ia_b < 0) ia_b = 0; else if (ia_b > 65535) ia_b = 65535;
                        int id_r = (int)(dr * 4096.0 + 0.5); if (id_r < 0) id_r = 0; else if (id_r > 65535) id_r = 65535;
                        int id_g = (int)(dg * 4096.0 + 0.5); if (id_g < 0) id_g = 0; else if (id_g > 65535) id_g = 65535;
                        int id_b = (int)(db * 4096.0 + 0.5); if (id_b < 0) id_b = 0; else if (id_b > 65535) id_b = 65535;
                        s_ped_amb_q12[aoi][0] = (uint16_t)ia_r;
                        s_ped_amb_q12[aoi][1] = (uint16_t)ia_g;
                        s_ped_amb_q12[aoi][2] = (uint16_t)ia_b;
                        s_ped_dir_q12[aoi][0] = (uint16_t)id_r;
                        s_ped_dir_q12[aoi][1] = (uint16_t)id_g;
                        s_ped_dir_q12[aoi][2] = (uint16_t)id_b;
                        s_ped_top_lut[aoi].r = ar + dr;
                        s_ped_top_lut[aoi].g = ag + dg;
                        s_ped_top_lut[aoi].b = ab + db;
                        s_ped_top_lut[aoi].a = 1.0;
                    }
                    s_ped_lut_nx = ped_last_nx;
                    s_ped_lut_ny = ped_last_ny;
                    s_ped_lut_nz = ped_last_nz;
                    s_ped_lut_mat = mat_idx;
                    s_ped_lut_shd = shd_on;
                    s_ped_lut_l1 = l1_on;
                    ped_lut_ready = true;
                }
                uint8_t shv = shdrow ? shdrow[x >> 1] : 0u;
                uint8_t aoi = grow[x].ao;
                if (shv == 0u) {
                    drow[x] = s_ped_top_lut[aoi];
                } else {
                    int32_t vis_q12 = 4096 - (((int32_t)shv * 3020) >> 8);
                    if (vis_q12 < 0) vis_q12 = 0;
                    int32_t pr = (int32_t)s_ped_amb_q12[aoi][0] + (((int32_t)s_ped_dir_q12[aoi][0] * vis_q12) >> 12);
                    int32_t pg = (int32_t)s_ped_amb_q12[aoi][1] + (((int32_t)s_ped_dir_q12[aoi][1] * vis_q12) >> 12);
                    int32_t pb = (int32_t)s_ped_amb_q12[aoi][2] + (((int32_t)s_ped_dir_q12[aoi][2] * vis_q12) >> 12);
                    if (pr < 0) pr = 0;
                    if (pg < 0) pg = 0;
                    if (pb < 0) pb = 0;
                    drow[x].r = (double)pr * (1.0 / 4096.0);
                    drow[x].g = (double)pg * (1.0 / 4096.0);
                    drow[x].b = (double)pb * (1.0 / 4096.0);
                    drow[x].a = 1.0;
                }
                continue;
            }
            drow[x] = (mat_id == 4u) ? ped_hdr_mat4 : ped_hdr_mat3;
        }
    }
}

typedef struct {
    int w;
    int ux0, uy0, ux1, uy1;
    int cx0, cy0, cx1, cy1;
    bool restore_bg;
    int shd_w;
    ipo_rgba64f_t *dst64;
    bool has_soft_shadow;
    bool l1_on;
    int32_t alb_diff_r_q12, alb_diff_g_q12, alb_diff_b_q12;
    int32_t f0_r_q12, f0_g_q12, f0_b_q12;
    int32_t env_s_q12, cc_q12;
    int32_t spec_tint_r_q12, spec_tint_g_q12, spec_tint_b_q12;
    const uint32_t *ggx_lut;
    const uint16_t *g1_lut;
    volatile int next_y;
} shade64_job_t;

static void shade64_worker(int tid, void *ctx) {
    (void)tid;
    shade64_job_t *j = (shade64_job_t *)ctx;
    const int w = j->w;
    const int ux0 = j->ux0, ux1 = j->ux1, uy1 = j->uy1;
    const int cx0 = j->cx0, cy0 = j->cy0, cx1 = j->cx1, cy1 = j->cy1;
    const bool restore_bg = j->restore_bg;
    const int shd_w = j->shd_w;
    ipo_rgba64f_t *dst64 = j->dst64;
    const bool has_soft_shadow = j->has_soft_shadow;
    const bool l1_on = j->l1_on;
    const int32_t alb_diff_r_q12 = j->alb_diff_r_q12;
    const int32_t alb_diff_g_q12 = j->alb_diff_g_q12;
    const int32_t alb_diff_b_q12 = j->alb_diff_b_q12;
    const int32_t f0_r_q12 = j->f0_r_q12, f0_g_q12 = j->f0_g_q12, f0_b_q12 = j->f0_b_q12;
    const int32_t env_s_q12 = j->env_s_q12, cc_q12 = j->cc_q12;
    const int32_t spec_tint_r_q12 = j->spec_tint_r_q12;
    const int32_t spec_tint_g_q12 = j->spec_tint_g_q12;
    const int32_t spec_tint_b_q12 = j->spec_tint_b_q12;
    const uint32_t *ggx_lut = j->ggx_lut;
    const uint16_t *g1_lut = j->g1_lut;

    for (;;) {
        int y0 = __atomic_fetch_add(&j->next_y, 4, __ATOMIC_RELAXED);
        if (y0 > uy1) break;
        int y1 = y0 + 3;
        if (y1 > uy1) y1 = uy1;

        for (int y = y0; y <= y1; y++) {
            size_t row_off = (size_t)y * (size_t)w;
            ipo_rgba64f_t *drow = &dst64[row_off];
            const ipo_rgba64f_t *bgrow = &s_bg_hdr_cache[row_off];

            if (cx1 < cx0 || y < cy0 || y > cy1) {
                if (restore_bg && ux1 >= ux0) {
                    memcpy(drow + ux0, bgrow + ux0, (size_t)(ux1 - ux0 + 1) * sizeof(ipo_rgba64f_t));
                }
                continue;
            }

            if (restore_bg && cx0 > ux0) {
                memcpy(drow + ux0, bgrow + ux0, (size_t)(cx0 - ux0) * sizeof(ipo_rgba64f_t));
            }
            if (restore_bg && ux1 > cx1) {
                memcpy(drow + cx1 + 1, bgrow + cx1 + 1, (size_t)(ux1 - cx1) * sizeof(ipo_rgba64f_t));
            }

            const gbuf_px_t *grow = &s_gbuf[row_off];
            const uint8_t *shdrow_half = (has_soft_shadow && s_shd_blur) ? &s_shd_blur[(size_t)(y >> 1) * (size_t)shd_w] : NULL;
            const int32_t vy_ray = (int32_t)s_vy_row_q14[y];
            const vdir_q14_t *vrow = s_vdir_q14 ? &s_vdir_q14[row_off] : NULL;

            for (int x = cx0; x <= cx1; x++) {
                uint8_t mat_id = grow[x].mat;
                if (mat_id == 1u) {
                    int32_t Nx, Ny, Nz;
                    norm_vec3_q14((int32_t)grow[x].nx, (int32_t)grow[x].ny, (int32_t)grow[x].nz, &Nx, &Ny, &Nz);

                    int32_t vx, vy, vz;
                    if (vrow) {
                        vx = vrow[x].x; vy = vrow[x].y; vz = vrow[x].z;
                    } else {
                        norm_vec3_q14((int32_t)s_vx_col_q14[x], vy_ray, 15872, &vx, &vy, &vz);
                    }

                    int32_t ndotv_q14 = (Nx * vx + Ny * vy + Nz * vz) >> 14;
                    if (ndotv_q14 < 0) ndotv_q14 = 0; else if (ndotv_q14 > 16384) ndotv_q14 = 16384;
                    int32_t ndotv_idx = ndotv_q14 >> 4;

                    int32_t gv_q12 = (int32_t)g1_lut[ndotv_idx];

                    int32_t ndotl0_q12 = (Nx * 8511 + Ny * 11777 + Nz * 7551) >> 16;
                    int32_t spec0_q12 = 0;
                    if (ndotl0_q12 > 0) {
                        if (ndotl0_q12 > 4096) ndotl0_q12 = 4096;
                        int32_t nh0_q12 = (Nx * 4992 + Ny * 6912 + Nz * 14017) >> 16;
                        if (nh0_q12 > 0) {
                            if (nh0_q12 > 4096) nh0_q12 = 4096;
                            int32_t nh0_idx = nh0_q12 >> 2;
                            uint32_t g0_q12 = ((uint32_t)gv_q12 * (uint32_t)g1_lut[ndotl0_q12 >> 2]) >> 12;
                            spec0_q12 = (int32_t)((ggx_lut[nh0_idx] * g0_q12) >> 12);
                            if (cc_q12) {
                                spec0_q12 += ((int32_t)s_cc_q12[nh0_idx] * cc_q12) >> 12;
                            }
                            if (spec0_q12 > 14336) spec0_q12 = 14336;
                        }
                    } else {
                        ndotl0_q12 = 0;
                    }

                    int32_t ndotl1_q12 = 0, spec1_q12 = 0;
                    if (l1_on) {
                        ndotl1_q12 = (Nx * (-11136) + Ny * 8511 + Nz * (-8511)) >> 16;
                        if (ndotl1_q12 > 0) {
                            if (ndotl1_q12 > 4096) ndotl1_q12 = 4096;
                            int32_t nh1_q12 = (Nx * (-9024) + Ny * 6912 + Nz * 11777) >> 16;
                            if (nh1_q12 > 0) {
                                if (nh1_q12 > 4096) nh1_q12 = 4096;
                                int32_t nh1_idx = nh1_q12 >> 2;
                                uint32_t g1_q12 = ((uint32_t)gv_q12 * (uint32_t)g1_lut[ndotl1_q12 >> 2]) >> 12;
                                uint32_t raw1 = (ggx_lut[nh1_idx] * g1_q12) >> 12;
                                spec1_q12 = (int32_t)((raw1 * 2085u) >> 12);
                                if (cc_q12) {
                                    spec1_q12 += ((int32_t)s_cc_q12[nh1_idx] * (cc_q12 >> 1)) >> 12;
                                }
                                if (spec1_q12 > 10240) spec1_q12 = 10240;
                            }
                        } else {
                            ndotl1_q12 = 0;
                        }
                    }

                    int32_t two_nv = ndotv_q14 << 1;
                    int32_t rx = ((two_nv * Nx) >> 14) - vx;
                    int32_t ry = ((two_nv * Ny) >> 14) - vy;
                    int32_t rz = ((two_nv * Nz) >> 14) - vz;
                    int32_t env[3];
                    sample_cubemap_q12(rx, ry, rz, env);

                    int32_t ao_q12 = ((int32_t)grow[x].ao + 1) << 4;
                    int32_t hemi_q12 = 2048 + (Ny >> 3);
                    if (hemi_q12 < 0) hemi_q12 = 0; else if (hemi_q12 > 4096) hemi_q12 = 4096;
                    int32_t ao_vis_q12 = 2048 + (ao_q12 >> 1);

                    int32_t irr_r = ((((512 + ((544 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                     ((((4735 * ndotl0_q12 + 1024 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));
                    int32_t irr_g = ((((480 + ((608 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                     ((((4481 * ndotl0_q12 + 1409 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));
                    int32_t irr_b = ((((480 + ((768 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                     ((((4030 * ndotl0_q12 + 2114 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));

                    int32_t env_ao_q12 = (env_s_q12 * ao_q12) >> 12;
                    int32_t f5_q12 = (int32_t)s_fresnel_q12[ndotv_idx];
                    int32_t fr_q12 = f0_r_q12 + (((4096 - f0_r_q12) * f5_q12) >> 12);
                    int32_t fg_q12 = f0_g_q12 + (((4096 - f0_g_q12) * f5_q12) >> 12);
                    int32_t fb_q12 = f0_b_q12 + (((4096 - f0_b_q12) * f5_q12) >> 12);

                    int32_t out_r = (((((4096 - fr_q12) * alb_diff_r_q12) >> 12) * irr_r +
                                      fr_q12 * ((env[0] * env_ao_q12) >> 12)) >> 12) +
                                    ((((spec0_q12 * 6554 + spec1_q12 * 1925) >> 12) * spec_tint_r_q12) >> 12);
                    int32_t out_g = (((((4096 - fg_q12) * alb_diff_g_q12) >> 12) * irr_g +
                                      fg_q12 * ((env[1] * env_ao_q12) >> 12)) >> 12) +
                                    ((((spec0_q12 * 6144 + spec1_q12 * 2499) >> 12) * spec_tint_g_q12) >> 12);
                    int32_t out_b = (((((4096 - fb_q12) * alb_diff_b_q12) >> 12) * irr_b +
                                      fb_q12 * ((env[2] * env_ao_q12) >> 12)) >> 12) +
                                    ((((spec0_q12 * 5571 + spec1_q12 * 3482) >> 12) * spec_tint_b_q12) >> 12);

                    drow[x].r = q12_to_f64_fast(out_r);
                    drow[x].g = q12_to_f64_fast(out_g);
                    drow[x].b = q12_to_f64_fast(out_b);
                    drow[x].a = 1.0;
                } else if (mat_id == 2u && shdrow_half) {
                    uint8_t shv = shdrow_half[x >> 1];
                    if (shv != 0u) {
                        uint8_t aoi = grow[x].ao;
                        int32_t vis_q12 = 4096 - (((int32_t)shv * 3020) >> 8);
                        if (vis_q12 < 0) vis_q12 = 0;
                        int32_t pr = (int32_t)s_ped_amb_q12[aoi][0] + (((int32_t)s_ped_dir_q12[aoi][0] * vis_q12) >> 12);
                        int32_t pg = (int32_t)s_ped_amb_q12[aoi][1] + (((int32_t)s_ped_dir_q12[aoi][1] * vis_q12) >> 12);
                        int32_t pb = (int32_t)s_ped_amb_q12[aoi][2] + (((int32_t)s_ped_dir_q12[aoi][2] * vis_q12) >> 12);
                        drow[x].r = q12_to_f64_fast(pr);
                        drow[x].g = q12_to_f64_fast(pg);
                        drow[x].b = q12_to_f64_fast(pb);
                        drow[x].a = 1.0;
                    } else if (restore_bg) {
                        drow[x] = bgrow[x];
                    }
                } else if (restore_bg) {
                    drow[x] = bgrow[x];
                }
            }
        }
    }
}

static void shade_deferred_pbr_rgba64f(int w, int h,
                                       ipo_rgba64f_t *dst64,
                                       int mat_idx,
                                       bool has_soft_shadow) {
    if (!dst64 || !s_gbuf || !s_bg_hdr_cache || !s_vx_col_q14 || !s_vy_row_q14) return;

    bool need_restore = true;
    if (!s_hdr_bg_primed) {
        memcpy(dst64, s_bg_hdr_cache, (size_t)w * (size_t)h * sizeof(ipo_rgba64f_t));
        s_hdr_bg_primed = true;
        s_dirty_full_frame = true;
        need_restore = false;
    } else {
        s_dirty_full_frame = false;
    }

    int ux0 = w, uy0 = h, ux1 = -1, uy1 = -1;
    if (s_prev_bx1 >= s_prev_bx0 && s_prev_by1 >= s_prev_by0) {
        if (s_prev_bx0 < ux0) ux0 = s_prev_bx0;
        if (s_prev_by0 < uy0) uy0 = s_prev_by0;
        if (s_prev_bx1 > ux1) ux1 = s_prev_bx1;
        if (s_prev_by1 > uy1) uy1 = s_prev_by1;
    }
    if (s_shd_full_prev_x1 >= s_shd_full_prev_x0 && s_shd_full_prev_y1 >= s_shd_full_prev_y0) {
        if (s_shd_full_prev_x0 < ux0) ux0 = s_shd_full_prev_x0;
        if (s_shd_full_prev_y0 < uy0) uy0 = s_shd_full_prev_y0;
        if (s_shd_full_prev_x1 > ux1) ux1 = s_shd_full_prev_x1;
        if (s_shd_full_prev_y1 > uy1) uy1 = s_shd_full_prev_y1;
    }

    int cx0 = w, cy0 = h, cx1 = -1, cy1 = -1;
    if (s_cur_bx1 >= s_cur_bx0 && s_cur_by1 >= s_cur_by0) {
        if (s_cur_bx0 < cx0) cx0 = s_cur_bx0;
        if (s_cur_by0 < cy0) cy0 = s_cur_by0;
        if (s_cur_bx1 > cx1) cx1 = s_cur_bx1;
        if (s_cur_by1 > cy1) cy1 = s_cur_by1;
    }
    if (has_soft_shadow && s_shd_full_cur_x1 >= s_shd_full_cur_x0 && s_shd_full_cur_y1 >= s_shd_full_cur_y0) {
        if (s_shd_full_cur_x0 < cx0) cx0 = s_shd_full_cur_x0;
        if (s_shd_full_cur_y0 < cy0) cy0 = s_shd_full_cur_y0;
        if (s_shd_full_cur_x1 > cx1) cx1 = s_shd_full_cur_x1;
        if (s_shd_full_cur_y1 > cy1) cy1 = s_shd_full_cur_y1;
    }
    if (cx1 >= cx0 && cy1 >= cy0) {
        if (cx0 < ux0) ux0 = cx0;
        if (cy0 < uy0) uy0 = cy0;
        if (cx1 > ux1) ux1 = cx1;
        if (cy1 > uy1) uy1 = cy1;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 >= w) cx1 = w - 1;
        if (cy1 >= h) cy1 = h - 1;
    }
    if (ux0 < 0) ux0 = 0;
    if (uy0 < 0) uy0 = 0;
    if (ux1 >= w) ux1 = w - 1;
    if (uy1 >= h) uy1 = h - 1;
    s_dirty_x0 = ux0; s_dirty_y0 = uy0; s_dirty_x1 = ux1; s_dirty_y1 = uy1;
    if (ux1 < ux0 || uy1 < uy0) return;

    const pbr_material_t *mat = &materials[mat_idx];
    const int32_t alb_r_q12 = (int32_t)(mat->albedo[0] * 4096.0f + 0.5f);
    const int32_t alb_g_q12 = (int32_t)(mat->albedo[1] * 4096.0f + 0.5f);
    const int32_t alb_b_q12 = (int32_t)(mat->albedo[2] * 4096.0f + 0.5f);
    const int32_t f0_r_q12  = (int32_t)(mat->f0[0] * 4096.0f + 0.5f);
    const int32_t f0_g_q12  = (int32_t)(mat->f0[1] * 4096.0f + 0.5f);
    const int32_t f0_b_q12  = (int32_t)(mat->f0[2] * 4096.0f + 0.5f);
    const int32_t metal_q12 = (int32_t)(mat->metallic * 4096.0f + 0.5f);
    const int32_t diff_w_q12 = (int32_t)((1.0f - mat->metallic * 0.92f) * 4096.0f + 0.5f);
    const int32_t alb_diff_r_q12 = (alb_r_q12 * diff_w_q12) >> 12;
    const int32_t alb_diff_g_q12 = (alb_g_q12 * diff_w_q12) >> 12;
    const int32_t alb_diff_b_q12 = (alb_b_q12 * diff_w_q12) >> 12;
    const int32_t env_s_q12 = (int32_t)(mat->env_strength * 4096.0f + 0.5f);
    const int32_t cc_q12 = (int32_t)(mat->clearcoat * 4096.0f + 0.5f);

    const int32_t spec_tint_r_q12 = 4096 - metal_q12 + ((metal_q12 * alb_r_q12) >> 12);
    const int32_t spec_tint_g_q12 = 4096 - metal_q12 + ((metal_q12 * alb_g_q12) >> 12);
    const int32_t spec_tint_b_q12 = 4096 - metal_q12 + ((metal_q12 * alb_b_q12) >> 12);

    const uint32_t *ggx_lut = s_ggx_q12[mat_idx];
    const uint16_t *g1_lut  = s_smith_g_q12[mat_idx];
    const bool l1_on = light1_enabled;
    const int shd_w = (w + 1) >> 1;

    shade64_job_t sj = {
        .w = w,
        .ux0 = ux0, .uy0 = uy0, .ux1 = ux1, .uy1 = uy1,
        .cx0 = cx0, .cy0 = cy0, .cx1 = cx1, .cy1 = cy1,
        .restore_bg = need_restore,
        .shd_w = shd_w,
        .dst64 = dst64,
        .has_soft_shadow = has_soft_shadow,
        .l1_on = l1_on,
        .alb_diff_r_q12 = alb_diff_r_q12,
        .alb_diff_g_q12 = alb_diff_g_q12,
        .alb_diff_b_q12 = alb_diff_b_q12,
        .f0_r_q12 = f0_r_q12, .f0_g_q12 = f0_g_q12, .f0_b_q12 = f0_b_q12,
        .env_s_q12 = env_s_q12, .cc_q12 = cc_q12,
        .spec_tint_r_q12 = spec_tint_r_q12,
        .spec_tint_g_q12 = spec_tint_g_q12,
        .spec_tint_b_q12 = spec_tint_b_q12,
        .ggx_lut = ggx_lut,
        .g1_lut = g1_lut,
        .next_y = uy0
    };
    tp_dispatch(shade64_worker, &sj);

    /* Composite planar mirror reflection onto visible obsidian pedestal top (mat == 2) */
    if (s_mirr_gbuf && s_mirr_bx1 >= s_mirr_bx0 && s_mirr_by1 >= s_mirr_by0) {
        int mx0 = s_mirr_bx0 < 0 ? 0 : s_mirr_bx0;
        int my0 = s_mirr_by0 < 0 ? 0 : s_mirr_by0;
        int mx1 = s_mirr_bx1 >= w ? (w - 1) : s_mirr_bx1;
        int my1 = s_mirr_by1 >= h ? (h - 1) : s_mirr_by1;
        for (int y = my0; y <= my1; y++) {
            size_t row_off = (size_t)y * (size_t)w;
            const gbuf_px_t *grow = &s_gbuf[row_off];
            const mirr_px_t *mrow = &s_mirr_gbuf[row_off];
            const vdir_q14_t *vrow = s_vdir_q14 ? &s_vdir_q14[row_off] : NULL;
            const int32_t vy_ray = (int32_t)s_vy_row_q14[y];
            ipo_rgba64f_t *drow = &dst64[row_off];

            for (int x = mx0; x <= mx1; x++) {
                if (grow[x].mat != 2u || !mrow[x].valid) continue;

                int32_t Nx, Ny, Nz;
                norm_vec3_q14((int32_t)mrow[x].nx, (int32_t)mrow[x].ny, (int32_t)mrow[x].nz, &Nx, &Ny, &Nz);
                int32_t vx, vy, vz;
                if (vrow) {
                    vx = vrow[x].x; vy = vrow[x].y; vz = vrow[x].z;
                } else {
                    norm_vec3_q14((int32_t)s_vx_col_q14[x], vy_ray, 15872, &vx, &vy, &vz);
                }

                int32_t ndotv_q14 = (Nx * vx + Ny * vy + Nz * vz) >> 14;
                if (ndotv_q14 < 0) ndotv_q14 = 0; else if (ndotv_q14 > 16384) ndotv_q14 = 16384;
                int32_t ndotv_idx = ndotv_q14 >> 4;
                int32_t gv_q12 = (int32_t)g1_lut[ndotv_idx];

                int32_t ndotl0_q12 = (Nx * 8511 + Ny * 11777 + Nz * 7551) >> 16;
                int32_t spec0_q12 = 0;
                if (ndotl0_q12 > 0) {
                    if (ndotl0_q12 > 4096) ndotl0_q12 = 4096;
                    int32_t nh0_q12 = (Nx * 4992 + Ny * 6912 + Nz * 14017) >> 16;
                    if (nh0_q12 > 0) {
                        if (nh0_q12 > 4096) nh0_q12 = 4096;
                        int32_t nh0_idx = nh0_q12 >> 2;
                        uint32_t g0_q12 = ((uint32_t)gv_q12 * (uint32_t)g1_lut[ndotl0_q12 >> 2]) >> 12;
                        spec0_q12 = (int32_t)((ggx_lut[nh0_idx] * g0_q12) >> 12);
                        if (cc_q12) spec0_q12 += ((int32_t)s_cc_q12[nh0_idx] * cc_q12) >> 12;
                        if (spec0_q12 > 14336) spec0_q12 = 14336;
                    }
                } else ndotl0_q12 = 0;

                int32_t ndotl1_q12 = 0, spec1_q12 = 0;
                if (l1_on) {
                    ndotl1_q12 = (Nx * (-11136) + Ny * 8511 + Nz * (-8511)) >> 16;
                    if (ndotl1_q12 > 0) {
                        if (ndotl1_q12 > 4096) ndotl1_q12 = 4096;
                        int32_t nh1_q12 = (Nx * (-9024) + Ny * 6912 + Nz * 11777) >> 16;
                        if (nh1_q12 > 0) {
                            if (nh1_q12 > 4096) nh1_q12 = 4096;
                            int32_t nh1_idx = nh1_q12 >> 2;
                            uint32_t g1_q12 = ((uint32_t)gv_q12 * (uint32_t)g1_lut[ndotl1_q12 >> 2]) >> 12;
                            uint32_t raw1 = (ggx_lut[nh1_idx] * g1_q12) >> 12;
                            spec1_q12 = (int32_t)((raw1 * 2085u) >> 12);
                            if (cc_q12) spec1_q12 += ((int32_t)s_cc_q12[nh1_idx] * (cc_q12 >> 1)) >> 12;
                            if (spec1_q12 > 10240) spec1_q12 = 10240;
                        }
                    } else ndotl1_q12 = 0;
                }

                int32_t two_nv = ndotv_q14 << 1;
                int32_t rx = ((two_nv * Nx) >> 14) - vx;
                int32_t ry = ((two_nv * Ny) >> 14) - vy;
                int32_t rz = ((two_nv * Nz) >> 14) - vz;
                int32_t env[3];
                sample_cubemap_q12(rx, ry, rz, env);

                int32_t ao_q12 = ((int32_t)mrow[x].ao + 1) << 4;
                int32_t hemi_q12 = 2048 + (Ny >> 3);
                if (hemi_q12 < 0) hemi_q12 = 0; else if (hemi_q12 > 4096) hemi_q12 = 4096;
                int32_t ao_vis_q12 = 2048 + (ao_q12 >> 1);

                int32_t irr_r = ((((512 + ((544 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                 ((((4735 * ndotl0_q12 + 1024 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));
                int32_t irr_g = ((((480 + ((608 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                 ((((4481 * ndotl0_q12 + 1409 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));
                int32_t irr_b = ((((480 + ((768 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                 ((((4030 * ndotl0_q12 + 2114 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));

                int32_t env_ao_q12 = (env_s_q12 * ao_q12) >> 12;
                int32_t f5_q12 = (int32_t)s_fresnel_q12[ndotv_idx];
                int32_t fr_q12 = f0_r_q12 + (((4096 - f0_r_q12) * f5_q12) >> 12);
                int32_t fg_q12 = f0_g_q12 + (((4096 - f0_g_q12) * f5_q12) >> 12);
                int32_t fb_q12 = f0_b_q12 + (((4096 - f0_b_q12) * f5_q12) >> 12);

                int32_t out_r = (((((4096 - fr_q12) * alb_diff_r_q12) >> 12) * irr_r +
                                  fr_q12 * ((env[0] * env_ao_q12) >> 12)) >> 12) +
                                ((((spec0_q12 * 6554 + spec1_q12 * 1925) >> 12) * spec_tint_r_q12) >> 12);
                int32_t out_g = (((((4096 - fg_q12) * alb_diff_g_q12) >> 12) * irr_g +
                                  fg_q12 * ((env[1] * env_ao_q12) >> 12)) >> 12) +
                                ((((spec0_q12 * 6144 + spec1_q12 * 2499) >> 12) * spec_tint_g_q12) >> 12);
                int32_t out_b = (((((4096 - fb_q12) * alb_diff_b_q12) >> 12) * irr_b +
                                  fb_q12 * ((env[2] * env_ao_q12) >> 12)) >> 12) +
                                ((((spec0_q12 * 5571 + spec1_q12 * 3482) >> 12) * spec_tint_b_q12) >> 12);

                double mr = q12_to_f64_fast(out_r);
                double mg = q12_to_f64_fast(out_g);
                double mb = q12_to_f64_fast(out_b);
                drow[x].r = drow[x].r * 0.62 + mr * 0.40;
                drow[x].g = drow[x].g * 0.62 + mg * 0.40;
                drow[x].b = drow[x].b * 0.62 + mb * 0.40;
            }
        }
    }
}

static inline float tp_aces_gamma(float x) {
    if (x <= 0.0f) return 0.0f;
    float t = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return gl_sqrtf(t);
}

static inline float tp_inv_aces_gamma(float g) {
    if (g <= 0.0f) return 0.0f;
    if (g >= 0.992f) g = 0.992f;
    float t = g * g;
    if (t >= 0.985f) t = 0.985f;
    float a = 2.51f - 2.43f * t;
    float b = 0.03f - 0.59f * t;
    float c = -0.14f * t;
    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) disc = 0.0f;
    float x = (-b + gl_sqrtf(disc)) / (2.0f * a);
    return (x > 0.0f) ? x : 0.0f;
}

/* Perceptual ACES-Gamma FXAA with Depth + Material Discontinuity Detection for CPU RGBA64F */
static void edge_aa_hdr64f(int w, int h, ipo_rgba64f_t *fb) {
    if (!fb || !s_gbuf || w < 6 || h < 6) return;
    int x0 = (s_dirty_x0 - 1 > 1) ? (s_dirty_x0 - 1) : 1;
    int y0 = (s_dirty_y0 - 1 > 1) ? (s_dirty_y0 - 1) : 1;
    int x1 = (s_dirty_x1 + 1 < w - 2) ? (s_dirty_x1 + 1) : (w - 2);
    int y1 = (s_dirty_y1 + 1 < h - 2) ? (s_dirty_y1 + 1) : (h - 2);
    if (x1 < x0 || y1 < y0) return;

    int tw = (x1 - x0 + 1) + 2;
    int th = (y1 - y0 + 1) + 2;
    size_t tile_n = (size_t)tw * (size_t)th;
    static float *s_aa_rgb = NULL;
    static float *s_aa_lum = NULL;
    static size_t s_aa_cap = 0u;
    if (tile_n > s_aa_cap) {
        free(s_aa_rgb); free(s_aa_lum);
        s_aa_rgb = (float *)malloc(tile_n * 3u * sizeof(float));
        s_aa_lum = (float *)malloc(tile_n * sizeof(float));
        if (!s_aa_rgb || !s_aa_lum) {
            free(s_aa_rgb); free(s_aa_lum);
            s_aa_rgb = s_aa_lum = NULL; s_aa_cap = 0u;
            return;
        }
        s_aa_cap = tile_n;
    }

    int ox = x0 - 1, oy = y0 - 1;
    for (int ty = 0; ty < th; ty++) {
        size_t srow = (size_t)(oy + ty) * (size_t)w + (size_t)ox;
        size_t drow = (size_t)ty * (size_t)tw;
        for (int tx = 0; tx < tw; tx++) {
            float r = (float)fb[srow + (size_t)tx].r;
            float g = (float)fb[srow + (size_t)tx].g;
            float b = (float)fb[srow + (size_t)tx].b;
            size_t di = drow + (size_t)tx;
            s_aa_rgb[di * 3u + 0u] = r;
            s_aa_rgb[di * 3u + 1u] = g;
            s_aa_rgb[di * 3u + 2u] = b;
            s_aa_lum[di] = tp_aces_gamma(0.2126f * r + 0.7152f * g + 0.0722f * b);
        }
    }

    for (int y = y0; y <= y1; y++) {
        int ty = y - oy;
        size_t f_roff = (size_t)y * (size_t)w;
        size_t t_roff = (size_t)ty * (size_t)tw;
        const gbuf_px_t *g_up = &s_gbuf[f_roff - (size_t)w];
        const gbuf_px_t *g_c  = &s_gbuf[f_roff];
        const gbuf_px_t *g_dn = &s_gbuf[f_roff + (size_t)w];
        const uint16_t  *z_up = s_zbuf ? &s_zbuf[f_roff - (size_t)w] : NULL;
        const uint16_t  *z_c  = s_zbuf ? &s_zbuf[f_roff]             : NULL;
        const uint16_t  *z_dn = s_zbuf ? &s_zbuf[f_roff + (size_t)w] : NULL;
        ipo_rgba64f_t   *dst  = &fb[f_roff];

        for (int x = x0; x <= x1; x++) {
            int tx = x - ox;
            size_t ti = t_roff + (size_t)tx;
            size_t tiN = ti - (size_t)tw, tiS = ti + (size_t)tw;
            size_t tiW = ti - 1u,         tiE = ti + 1u;

            float lM = s_aa_lum[ti];
            float lN = s_aa_lum[tiN], lS = s_aa_lum[tiS];
            float lW = s_aa_lum[tiW], lE = s_aa_lum[tiE];
            float lMax = lM; if (lN > lMax) lMax = lN; if (lS > lMax) lMax = lS; if (lW > lMax) lMax = lW; if (lE > lMax) lMax = lE;
            float lMin = lM; if (lN < lMin) lMin = lN; if (lS < lMin) lMin = lS; if (lW < lMin) lMin = lW; if (lE < lMin) lMin = lE;
            float lRange = lMax - lMin;

            uint8_t mc = g_c[x].mat;
            bool mat_edge = (mc != g_c[x - 1].mat || mc != g_c[x + 1].mat || mc != g_up[x].mat || mc != g_dn[x].mat);
            bool z_edge = false;
            if (z_c && !mat_edge) {
                int zc = (int)z_c[x];
                int dzN = abs(zc - (int)z_up[x]), dzS = abs(zc - (int)z_dn[x]);
                int dzW = abs(zc - (int)z_c[x - 1]), dzE = abs(zc - (int)z_c[x + 1]);
                int dzM = dzN; if (dzS > dzM) dzM = dzS; if (dzW > dzM) dzM = dzW; if (dzE > dzM) dzM = dzE;
                z_edge = (dzM > 900);
            }
            float lThr = lMax * 0.095f; if (lThr < 0.036f) lThr = 0.036f;
            if (!mat_edge && !z_edge && lRange < lThr) continue;

            float lNW = s_aa_lum[tiN - 1u], lNE = s_aa_lum[tiN + 1u];
            float lSW = s_aa_lum[tiS - 1u], lSE = s_aa_lum[tiS + 1u];
            float lCross = 0.25f * (lN + lS + lW + lE);
            float subpix = (lRange > 1e-4f) ? (fabsf(lCross - lM) / lRange) : 0.42f;
            if (subpix > 1.0f) subpix = 1.0f;
            subpix = subpix * subpix * (3.0f - 2.0f * subpix) * (mat_edge ? 0.62f : 0.50f);

            float edgeH = fabsf(-2.0f * lW + lNW + lSW) + 2.0f * fabsf(-2.0f * lM + lN + lS) + fabsf(-2.0f * lE + lNE + lSE);
            float edgeV = fabsf(-2.0f * lN + lNW + lNE) + 2.0f * fabsf(-2.0f * lM + lW + lE) + fabsf(-2.0f * lS + lSW + lSE);
            bool horz = (edgeH >= edgeV);

            float lNeg = horz ? lN : lW, lPos = horz ? lS : lE;
            float gNeg = fabsf(lNeg - lM), gPos = fabsf(lPos - lM);
            bool useNeg = (gNeg >= gPos);
            size_t iOther = horz ? (useNeg ? tiN : tiS) : (useNeg ? tiW : tiE);
            float lOther = useNeg ? lNeg : lPos;
            float lAvg = 0.5f * (lM + lOther);
            float gTol = 0.25f * (gNeg > gPos ? gNeg : gPos);
            if (gTol < 0.015f) gTol = 0.015f;

            int dxt = horz ? 1 : 0, dyt = horz ? 0 : 1;
            int pd = useNeg ? -1 : 1;
            int dn = 5, dp = 5;
            float endN = lM, endP = lM;
            for (int s = 1; s <= 5; s++) {
                int stx = tx - dxt * s, sty = ty - dyt * s;
                if (stx < 0 || sty < 0) { dn = s; break; }
                int px2 = horz ? stx : ((stx + pd < 0 || stx + pd >= tw) ? stx : stx + pd);
                int py2 = horz ? ((sty + pd < 0 || sty + pd >= th) ? sty : sty + pd) : sty;
                float ln = 0.5f * (s_aa_lum[(size_t)sty * (size_t)tw + (size_t)stx] + s_aa_lum[(size_t)py2 * (size_t)tw + (size_t)px2]);
                endN = ln;
                if (fabsf(ln - lAvg) >= gTol) { dn = s; break; }
            }
            for (int s = 1; s <= 5; s++) {
                int stx = tx + dxt * s, sty = ty + dyt * s;
                if (stx >= tw || sty >= th) { dp = s; break; }
                int px2 = horz ? stx : ((stx + pd < 0 || stx + pd >= tw) ? stx : stx + pd);
                int py2 = horz ? ((sty + pd < 0 || sty + pd >= th) ? sty : sty + pd) : sty;
                float lp = 0.5f * (s_aa_lum[(size_t)sty * (size_t)tw + (size_t)stx] + s_aa_lum[(size_t)py2 * (size_t)tw + (size_t)px2]);
                endP = lp;
                if (fabsf(lp - lAvg) >= gTol) { dp = s; break; }
            }

            float edgeBlend = 0.0f;
            int dNear = (dn < dp) ? dn : dp;
            float endNear = (dn < dp) ? endN : endP;
            if (((endNear - lAvg) < 0.0f) != ((lM - lAvg) < 0.0f)) {
                edgeBlend = 0.5f - (float)dNear / (float)(dn + dp);
                if (edgeBlend < 0.0f) edgeBlend = 0.0f;
            }
            float blend = (subpix > edgeBlend) ? subpix : edgeBlend;
            if (blend > 0.72f) blend = 0.72f;
            if (blend > 0.005f) {
                float omb = 1.0f - blend;
                float mr = tp_aces_gamma(s_aa_rgb[ti * 3u + 0u]) * omb + tp_aces_gamma(s_aa_rgb[iOther * 3u + 0u]) * blend;
                float mg = tp_aces_gamma(s_aa_rgb[ti * 3u + 1u]) * omb + tp_aces_gamma(s_aa_rgb[iOther * 3u + 1u]) * blend;
                float mb = tp_aces_gamma(s_aa_rgb[ti * 3u + 2u]) * omb + tp_aces_gamma(s_aa_rgb[iOther * 3u + 2u]) * blend;
                dst[x].r = (double)tp_inv_aces_gamma(mr);
                dst[x].g = (double)tp_inv_aces_gamma(mg);
                dst[x].b = (double)tp_inv_aces_gamma(mb);
            }
        }
    }
}

static inline uint32_t tp_q12_to_xrgb32(int32_t r_q12, int32_t g_q12, int32_t b_q12) {
    int32_t rq8 = r_q12 >> 4;
    int32_t gq8 = g_q12 >> 4;
    int32_t bq8 = b_q12 >> 4;
    if (rq8 < 0) rq8 = 0; else if (rq8 >= HDR_Q8_MAX) rq8 = HDR_Q8_MAX - 1;
    if (gq8 < 0) gq8 = 0; else if (gq8 >= HDR_Q8_MAX) gq8 = HDR_Q8_MAX - 1;
    if (bq8 < 0) bq8 = 0; else if (bq8 >= HDR_Q8_MAX) bq8 = HDR_Q8_MAX - 1;
    return 0xFF000000u |
           ((uint32_t)s_aces_lut[rq8] << 16) |
           ((uint32_t)s_aces_lut[gq8] << 8) |
           (uint32_t)s_aces_lut[bq8];
}

static uint32_t *s_xrgb_fb = NULL;
static uint32_t *s_bg_xrgb_cache = NULL;
static size_t    s_xrgb_cap = 0u;
static size_t    s_bg_xrgb_cap = 0u;

typedef struct {
    int w;
    int ux0, uy0, ux1, uy1;
    int cx0, cy0, cx1, cy1;
    bool restore_bg;
    int shd_w;
    uint32_t *dst32;
    const uint32_t *bg32;
    bool has_soft_shadow;
    bool l1_on;
    int32_t alb_diff_r_q12, alb_diff_g_q12, alb_diff_b_q12;
    int32_t f0_r_q12, f0_g_q12, f0_b_q12;
    int32_t env_s_q12, cc_q12;
    int32_t spec_tint_r_q12, spec_tint_g_q12, spec_tint_b_q12;
    const uint32_t *ggx_lut;
    const uint16_t *g1_lut;
    volatile int next_y;
} shade32_job_t;

static void shade32_worker(int tid, void *ctx) {
    (void)tid;
    shade32_job_t *j = (shade32_job_t *)ctx;
    const int w = j->w;
    const int ux0 = j->ux0, ux1 = j->ux1, uy1 = j->uy1;
    const int cx0 = j->cx0, cy0 = j->cy0, cx1 = j->cx1, cy1 = j->cy1;
    const bool restore_bg = j->restore_bg;
    const int shd_w = j->shd_w;
    uint32_t *dst32 = j->dst32;
    const uint32_t *bg_base = j->bg32;
    const bool has_soft_shadow = j->has_soft_shadow;
    const bool l1_on = j->l1_on;
    const int32_t alb_diff_r_q12 = j->alb_diff_r_q12;
    const int32_t alb_diff_g_q12 = j->alb_diff_g_q12;
    const int32_t alb_diff_b_q12 = j->alb_diff_b_q12;
    const int32_t f0_r_q12 = j->f0_r_q12, f0_g_q12 = j->f0_g_q12, f0_b_q12 = j->f0_b_q12;
    const int32_t env_s_q12 = j->env_s_q12, cc_q12 = j->cc_q12;
    const int32_t spec_tint_r_q12 = j->spec_tint_r_q12;
    const int32_t spec_tint_g_q12 = j->spec_tint_g_q12;
    const int32_t spec_tint_b_q12 = j->spec_tint_b_q12;
    const uint32_t *ggx_lut = j->ggx_lut;
    const uint16_t *g1_lut = j->g1_lut;

    for (;;) {
        int y0 = __atomic_fetch_add(&j->next_y, 4, __ATOMIC_RELAXED);
        if (y0 > uy1) break;
        int y1 = y0 + 3;
        if (y1 > uy1) y1 = uy1;

        for (int y = y0; y <= y1; y++) {
            size_t row_off = (size_t)y * (size_t)w;
            uint32_t *drow = &dst32[row_off];
            const uint32_t *bgrow = &bg_base[row_off];

            if (cx1 < cx0 || y < cy0 || y > cy1) {
                if (restore_bg && ux1 >= ux0) {
                    memcpy(drow + ux0, bgrow + ux0, (size_t)(ux1 - ux0 + 1) * sizeof(uint32_t));
                }
                continue;
            }

            if (restore_bg && cx0 > ux0) {
                memcpy(drow + ux0, bgrow + ux0, (size_t)(cx0 - ux0) * sizeof(uint32_t));
            }
            if (restore_bg && ux1 > cx1) {
                memcpy(drow + cx1 + 1, bgrow + cx1 + 1, (size_t)(ux1 - cx1) * sizeof(uint32_t));
            }

            const gbuf_px_t *grow = &s_gbuf[row_off];
            const uint8_t *shdrow_half = (has_soft_shadow && s_shd_blur) ? &s_shd_blur[(size_t)(y >> 1) * (size_t)shd_w] : NULL;
            const int32_t vy_ray = (int32_t)s_vy_row_q14[y];
            const vdir_q14_t *vrow = s_vdir_q14 ? &s_vdir_q14[row_off] : NULL;

            for (int x = cx0; x <= cx1; x++) {
                uint8_t mat_id = grow[x].mat;
                if (mat_id == 1u) {
                    int32_t Nx, Ny, Nz;
                    norm_vec3_q14((int32_t)grow[x].nx, (int32_t)grow[x].ny, (int32_t)grow[x].nz, &Nx, &Ny, &Nz);

                    int32_t vx, vy, vz;
                    if (vrow) {
                        vx = vrow[x].x; vy = vrow[x].y; vz = vrow[x].z;
                    } else {
                        norm_vec3_q14((int32_t)s_vx_col_q14[x], vy_ray, 15872, &vx, &vy, &vz);
                    }

                    int32_t ndotv_q14 = (Nx * vx + Ny * vy + Nz * vz) >> 14;
                    if (ndotv_q14 < 0) ndotv_q14 = 0; else if (ndotv_q14 > 16384) ndotv_q14 = 16384;
                    int32_t ndotv_idx = ndotv_q14 >> 4;
                    int32_t gv_q12 = (int32_t)g1_lut[ndotv_idx];

                    int32_t ndotl0_q12 = (Nx * 8511 + Ny * 11777 + Nz * 7551) >> 16;
                    int32_t spec0_q12 = 0;
                    if (ndotl0_q12 > 0) {
                        if (ndotl0_q12 > 4096) ndotl0_q12 = 4096;
                        int32_t nh0_q12 = (Nx * 4992 + Ny * 6912 + Nz * 14017) >> 16;
                        if (nh0_q12 > 0) {
                            if (nh0_q12 > 4096) nh0_q12 = 4096;
                            int32_t nh0_idx = nh0_q12 >> 2;
                            uint32_t g0_q12 = ((uint32_t)gv_q12 * (uint32_t)g1_lut[ndotl0_q12 >> 2]) >> 12;
                            spec0_q12 = (int32_t)((ggx_lut[nh0_idx] * g0_q12) >> 12);
                            if (cc_q12) spec0_q12 += ((int32_t)s_cc_q12[nh0_idx] * cc_q12) >> 12;
                            if (spec0_q12 > 14336) spec0_q12 = 14336;
                        }
                    } else {
                        ndotl0_q12 = 0;
                    }

                    int32_t ndotl1_q12 = 0, spec1_q12 = 0;
                    if (l1_on) {
                        ndotl1_q12 = (Nx * (-11136) + Ny * 8511 + Nz * (-8511)) >> 16;
                        if (ndotl1_q12 > 0) {
                            if (ndotl1_q12 > 4096) ndotl1_q12 = 4096;
                            int32_t nh1_q12 = (Nx * (-9024) + Ny * 6912 + Nz * 11777) >> 16;
                            if (nh1_q12 > 0) {
                                if (nh1_q12 > 4096) nh1_q12 = 4096;
                                int32_t nh1_idx = nh1_q12 >> 2;
                                uint32_t g1_q12 = ((uint32_t)gv_q12 * (uint32_t)g1_lut[ndotl1_q12 >> 2]) >> 12;
                                uint32_t raw1 = (ggx_lut[nh1_idx] * g1_q12) >> 12;
                                spec1_q12 = (int32_t)((raw1 * 2085u) >> 12);
                                if (cc_q12) spec1_q12 += ((int32_t)s_cc_q12[nh1_idx] * (cc_q12 >> 1)) >> 12;
                                if (spec1_q12 > 10240) spec1_q12 = 10240;
                            }
                        } else {
                            ndotl1_q12 = 0;
                        }
                    }

                    int32_t two_nv = ndotv_q14 << 1;
                    int32_t rx = ((two_nv * Nx) >> 14) - vx;
                    int32_t ry = ((two_nv * Ny) >> 14) - vy;
                    int32_t rz = ((two_nv * Nz) >> 14) - vz;
                    int32_t env[3];
                    sample_cubemap_q12(rx, ry, rz, env);

                    int32_t ao_q12 = ((int32_t)grow[x].ao + 1) << 4;
                    int32_t hemi_q12 = 2048 + (Ny >> 3);
                    if (hemi_q12 < 0) hemi_q12 = 0; else if (hemi_q12 > 4096) hemi_q12 = 4096;
                    int32_t ao_vis_q12 = 2048 + (ao_q12 >> 1);

                    int32_t irr_r = ((((512 + ((544 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                     ((((4735 * ndotl0_q12 + 1024 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));
                    int32_t irr_g = ((((480 + ((608 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                     ((((4481 * ndotl0_q12 + 1409 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));
                    int32_t irr_b = ((((480 + ((768 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                     ((((4030 * ndotl0_q12 + 2114 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));

                    int32_t env_ao_q12 = (env_s_q12 * ao_q12) >> 12;
                    int32_t f5_q12 = (int32_t)s_fresnel_q12[ndotv_idx];
                    int32_t fr_q12 = f0_r_q12 + (((4096 - f0_r_q12) * f5_q12) >> 12);
                    int32_t fg_q12 = f0_g_q12 + (((4096 - f0_g_q12) * f5_q12) >> 12);
                    int32_t fb_q12 = f0_b_q12 + (((4096 - f0_b_q12) * f5_q12) >> 12);

                    int32_t out_r = (((((4096 - fr_q12) * alb_diff_r_q12) >> 12) * irr_r +
                                      fr_q12 * ((env[0] * env_ao_q12) >> 12)) >> 12) +
                                    ((((spec0_q12 * 6554 + spec1_q12 * 1925) >> 12) * spec_tint_r_q12) >> 12);
                    int32_t out_g = (((((4096 - fg_q12) * alb_diff_g_q12) >> 12) * irr_g +
                                      fg_q12 * ((env[1] * env_ao_q12) >> 12)) >> 12) +
                                    ((((spec0_q12 * 6144 + spec1_q12 * 2499) >> 12) * spec_tint_g_q12) >> 12);
                    int32_t out_b = (((((4096 - fb_q12) * alb_diff_b_q12) >> 12) * irr_b +
                                      fb_q12 * ((env[2] * env_ao_q12) >> 12)) >> 12) +
                                    ((((spec0_q12 * 5571 + spec1_q12 * 3482) >> 12) * spec_tint_b_q12) >> 12);

                    drow[x] = tp_q12_to_xrgb32(out_r, out_g, out_b);
                } else if (mat_id == 2u && shdrow_half) {
                    uint8_t shv = shdrow_half[x >> 1];
                    if (shv != 0u) {
                        uint8_t aoi = grow[x].ao;
                        int32_t vis_q12 = 4096 - (((int32_t)shv * 3020) >> 8);
                        if (vis_q12 < 0) vis_q12 = 0;
                        int32_t pr = (int32_t)s_ped_amb_q12[aoi][0] + (((int32_t)s_ped_dir_q12[aoi][0] * vis_q12) >> 12);
                        int32_t pg = (int32_t)s_ped_amb_q12[aoi][1] + (((int32_t)s_ped_dir_q12[aoi][1] * vis_q12) >> 12);
                        int32_t pb = (int32_t)s_ped_amb_q12[aoi][2] + (((int32_t)s_ped_dir_q12[aoi][2] * vis_q12) >> 12);
                        drow[x] = tp_q12_to_xrgb32(pr, pg, pb);
                    } else if (restore_bg) {
                        drow[x] = bgrow[x];
                    }
                } else if (restore_bg) {
                    drow[x] = bgrow[x];
                }
            }
        }
    }
}

static bool shade_deferred_pbr_xrgb32(int w, int h, int fb_h, int mat_idx, bool has_soft_shadow) {
    if (!s_gbuf || !s_bg_hdr_cache || !s_vx_col_q14 || !s_vy_row_q14) return false;
    int full_h = (fb_h > h) ? fb_h : h;
    size_t need = (size_t)w * (size_t)full_h;
    if (need > s_xrgb_cap) {
        uint32_t *nb = (uint32_t *)realloc(s_xrgb_fb, need * sizeof(uint32_t));
        if (!nb) return false;
        s_xrgb_fb = nb;
        s_xrgb_cap = need;
        s_hdr_bg_primed = false;
    }
    if (need > s_bg_xrgb_cap) {
        uint32_t *nbg = (uint32_t *)realloc(s_bg_xrgb_cache, need * sizeof(uint32_t));
        if (!nbg) return false;
        s_bg_xrgb_cache = nbg;
        s_bg_xrgb_cap = need;
        s_hdr_bg_primed = false;
    }

    bool need_restore = true;
    if (!s_hdr_bg_primed) {
        size_t total_px = (size_t)w * (size_t)h;
        for (size_t i = 0u; i < total_px; i++) {
            int32_t rq8 = (int32_t)(s_bg_hdr_cache[i].r * 256.0 + 0.5);
            int32_t gq8 = (int32_t)(s_bg_hdr_cache[i].g * 256.0 + 0.5);
            int32_t bq8 = (int32_t)(s_bg_hdr_cache[i].b * 256.0 + 0.5);
            if (rq8 < 0) rq8 = 0; else if (rq8 >= HDR_Q8_MAX) rq8 = HDR_Q8_MAX - 1;
            if (gq8 < 0) gq8 = 0; else if (gq8 >= HDR_Q8_MAX) gq8 = HDR_Q8_MAX - 1;
            if (bq8 < 0) bq8 = 0; else if (bq8 >= HDR_Q8_MAX) bq8 = HDR_Q8_MAX - 1;
            uint32_t c = 0xFF000000u |
                         ((uint32_t)s_aces_lut[rq8] << 16) |
                         ((uint32_t)s_aces_lut[gq8] << 8) |
                         (uint32_t)s_aces_lut[bq8];
            s_bg_xrgb_cache[i] = c;
            s_xrgb_fb[i] = c;
        }
        s_hdr_bg_primed = true;
        s_dirty_full_frame = true;
        need_restore = false;
    } else {
        s_dirty_full_frame = false;
    }

    int ux0 = w, uy0 = h, ux1 = -1, uy1 = -1;
    if (s_prev_bx1 >= s_prev_bx0 && s_prev_by1 >= s_prev_by0) {
        if (s_prev_bx0 < ux0) ux0 = s_prev_bx0;
        if (s_prev_by0 < uy0) uy0 = s_prev_by0;
        if (s_prev_bx1 > ux1) ux1 = s_prev_bx1;
        if (s_prev_by1 > uy1) uy1 = s_prev_by1;
    }
    if (s_shd_full_prev_x1 >= s_shd_full_prev_x0 && s_shd_full_prev_y1 >= s_shd_full_prev_y0) {
        if (s_shd_full_prev_x0 < ux0) ux0 = s_shd_full_prev_x0;
        if (s_shd_full_prev_y0 < uy0) uy0 = s_shd_full_prev_y0;
        if (s_shd_full_prev_x1 > ux1) ux1 = s_shd_full_prev_x1;
        if (s_shd_full_prev_y1 > uy1) uy1 = s_shd_full_prev_y1;
    }

    int cx0 = w, cy0 = h, cx1 = -1, cy1 = -1;
    if (s_cur_bx1 >= s_cur_bx0 && s_cur_by1 >= s_cur_by0) {
        if (s_cur_bx0 < cx0) cx0 = s_cur_bx0;
        if (s_cur_by0 < cy0) cy0 = s_cur_by0;
        if (s_cur_bx1 > cx1) cx1 = s_cur_bx1;
        if (s_cur_by1 > cy1) cy1 = s_cur_by1;
    }
    if (has_soft_shadow && s_shd_full_cur_x1 >= s_shd_full_cur_x0 && s_shd_full_cur_y1 >= s_shd_full_cur_y0) {
        if (s_shd_full_cur_x0 < cx0) cx0 = s_shd_full_cur_x0;
        if (s_shd_full_cur_y0 < cy0) cy0 = s_shd_full_cur_y0;
        if (s_shd_full_cur_x1 > cx1) cx1 = s_shd_full_cur_x1;
        if (s_shd_full_cur_y1 > cy1) cy1 = s_shd_full_cur_y1;
    }
    if (cx1 >= cx0 && cy1 >= cy0) {
        if (cx0 < ux0) ux0 = cx0;
        if (cy0 < uy0) uy0 = cy0;
        if (cx1 > ux1) ux1 = cx1;
        if (cy1 > uy1) uy1 = cy1;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 >= w) cx1 = w - 1;
        if (cy1 >= h) cy1 = h - 1;
    }
    if (ux0 < 0) ux0 = 0;
    if (uy0 < 0) uy0 = 0;
    if (ux1 >= w) ux1 = w - 1;
    if (uy1 >= h) uy1 = h - 1;
    s_dirty_x0 = ux0; s_dirty_y0 = uy0; s_dirty_x1 = ux1; s_dirty_y1 = uy1;
    if (ux1 < ux0 || uy1 < uy0) return true;

    const pbr_material_t *mat = &materials[mat_idx];
    const int32_t alb_r_q12 = (int32_t)(mat->albedo[0] * 4096.0f + 0.5f);
    const int32_t alb_g_q12 = (int32_t)(mat->albedo[1] * 4096.0f + 0.5f);
    const int32_t alb_b_q12 = (int32_t)(mat->albedo[2] * 4096.0f + 0.5f);
    const int32_t f0_r_q12  = (int32_t)(mat->f0[0] * 4096.0f + 0.5f);
    const int32_t f0_g_q12  = (int32_t)(mat->f0[1] * 4096.0f + 0.5f);
    const int32_t f0_b_q12  = (int32_t)(mat->f0[2] * 4096.0f + 0.5f);
    const int32_t metal_q12 = (int32_t)(mat->metallic * 4096.0f + 0.5f);
    const int32_t diff_w_q12 = (int32_t)((1.0f - mat->metallic * 0.92f) * 4096.0f + 0.5f);
    const int32_t alb_diff_r_q12 = (alb_r_q12 * diff_w_q12) >> 12;
    const int32_t alb_diff_g_q12 = (alb_g_q12 * diff_w_q12) >> 12;
    const int32_t alb_diff_b_q12 = (alb_b_q12 * diff_w_q12) >> 12;
    const int32_t env_s_q12 = (int32_t)(mat->env_strength * 4096.0f + 0.5f);
    const int32_t cc_q12 = (int32_t)(mat->clearcoat * 4096.0f + 0.5f);

    const int32_t spec_tint_r_q12 = 4096 - metal_q12 + ((metal_q12 * alb_r_q12) >> 12);
    const int32_t spec_tint_g_q12 = 4096 - metal_q12 + ((metal_q12 * alb_g_q12) >> 12);
    const int32_t spec_tint_b_q12 = 4096 - metal_q12 + ((metal_q12 * alb_b_q12) >> 12);

    const uint32_t *ggx_lut = s_ggx_q12[mat_idx];
    const uint16_t *g1_lut  = s_smith_g_q12[mat_idx];
    const bool l1_on = light1_enabled;
    const int shd_w = (w + 1) >> 1;

    shade32_job_t sj = {
        .w = w,
        .ux0 = ux0, .uy0 = uy0, .ux1 = ux1, .uy1 = uy1,
        .cx0 = cx0, .cy0 = cy0, .cx1 = cx1, .cy1 = cy1,
        .restore_bg = need_restore,
        .shd_w = shd_w,
        .dst32 = s_xrgb_fb,
        .bg32 = s_bg_xrgb_cache,
        .has_soft_shadow = has_soft_shadow,
        .l1_on = l1_on,
        .alb_diff_r_q12 = alb_diff_r_q12,
        .alb_diff_g_q12 = alb_diff_g_q12,
        .alb_diff_b_q12 = alb_diff_b_q12,
        .f0_r_q12 = f0_r_q12, .f0_g_q12 = f0_g_q12, .f0_b_q12 = f0_b_q12,
        .env_s_q12 = env_s_q12, .cc_q12 = cc_q12,
        .spec_tint_r_q12 = spec_tint_r_q12,
        .spec_tint_g_q12 = spec_tint_g_q12,
        .spec_tint_b_q12 = spec_tint_b_q12,
        .ggx_lut = ggx_lut,
        .g1_lut = g1_lut,
        .next_y = uy0
    };
    tp_dispatch(shade32_worker, &sj);

    if (s_mirr_gbuf && s_mirr_bx1 >= s_mirr_bx0 && s_mirr_by1 >= s_mirr_by0) {
        int mx0 = s_mirr_bx0 < 0 ? 0 : s_mirr_bx0;
        int my0 = s_mirr_by0 < 0 ? 0 : s_mirr_by0;
        int mx1 = s_mirr_bx1 >= w ? (w - 1) : s_mirr_bx1;
        int my1 = s_mirr_by1 >= h ? (h - 1) : s_mirr_by1;
        for (int y = my0; y <= my1; y++) {
            size_t row_off = (size_t)y * (size_t)w;
            const gbuf_px_t *grow = &s_gbuf[row_off];
            const mirr_px_t *mrow = &s_mirr_gbuf[row_off];
            const uint8_t *shdrow_half = (has_soft_shadow && s_shd_blur) ? &s_shd_blur[(size_t)(y >> 1) * (size_t)shd_w] : NULL;
            const vdir_q14_t *vrow = s_vdir_q14 ? &s_vdir_q14[row_off] : NULL;
            const int32_t vy_ray = (int32_t)s_vy_row_q14[y];
            uint32_t *drow = &s_xrgb_fb[row_off];

            for (int x = mx0; x <= mx1; x++) {
                if (grow[x].mat != 2u || !mrow[x].valid) continue;

                int32_t Nx, Ny, Nz;
                norm_vec3_q14((int32_t)mrow[x].nx, (int32_t)mrow[x].ny, (int32_t)mrow[x].nz, &Nx, &Ny, &Nz);
                int32_t vx, vy, vz;
                if (vrow) {
                    vx = vrow[x].x; vy = vrow[x].y; vz = vrow[x].z;
                } else {
                    norm_vec3_q14((int32_t)s_vx_col_q14[x], vy_ray, 15872, &vx, &vy, &vz);
                }

                int32_t ndotv_q14 = (Nx * vx + Ny * vy + Nz * vz) >> 14;
                if (ndotv_q14 < 0) ndotv_q14 = 0; else if (ndotv_q14 > 16384) ndotv_q14 = 16384;
                int32_t ndotv_idx = ndotv_q14 >> 4;
                int32_t gv_q12 = (int32_t)g1_lut[ndotv_idx];

                int32_t ndotl0_q12 = (Nx * 8511 + Ny * 11777 + Nz * 7551) >> 16;
                int32_t spec0_q12 = 0;
                if (ndotl0_q12 > 0) {
                    if (ndotl0_q12 > 4096) ndotl0_q12 = 4096;
                    int32_t nh0_q12 = (Nx * 4992 + Ny * 6912 + Nz * 14017) >> 16;
                    if (nh0_q12 > 0) {
                        if (nh0_q12 > 4096) nh0_q12 = 4096;
                        int32_t nh0_idx = nh0_q12 >> 2;
                        uint32_t g0_q12 = ((uint32_t)gv_q12 * (uint32_t)g1_lut[ndotl0_q12 >> 2]) >> 12;
                        spec0_q12 = (int32_t)((ggx_lut[nh0_idx] * g0_q12) >> 12);
                        if (cc_q12) spec0_q12 += ((int32_t)s_cc_q12[nh0_idx] * cc_q12) >> 12;
                        if (spec0_q12 > 14336) spec0_q12 = 14336;
                    }
                } else ndotl0_q12 = 0;

                int32_t ndotl1_q12 = 0, spec1_q12 = 0;
                if (l1_on) {
                    ndotl1_q12 = (Nx * (-11136) + Ny * 8511 + Nz * (-8511)) >> 16;
                    if (ndotl1_q12 > 0) {
                        if (ndotl1_q12 > 4096) ndotl1_q12 = 4096;
                        int32_t nh1_q12 = (Nx * (-9024) + Ny * 6912 + Nz * 11777) >> 16;
                        if (nh1_q12 > 0) {
                            if (nh1_q12 > 4096) nh1_q12 = 4096;
                            int32_t nh1_idx = nh1_q12 >> 2;
                            uint32_t g1_q12 = ((uint32_t)gv_q12 * (uint32_t)g1_lut[ndotl1_q12 >> 2]) >> 12;
                            uint32_t raw1 = (ggx_lut[nh1_idx] * g1_q12) >> 12;
                            spec1_q12 = (int32_t)((raw1 * 2085u) >> 12);
                            if (cc_q12) spec1_q12 += ((int32_t)s_cc_q12[nh1_idx] * (cc_q12 >> 1)) >> 12;
                            if (spec1_q12 > 10240) spec1_q12 = 10240;
                        }
                    } else ndotl1_q12 = 0;
                }

                int32_t two_nv = ndotv_q14 << 1;
                int32_t rx = ((two_nv * Nx) >> 14) - vx;
                int32_t ry = ((two_nv * Ny) >> 14) - vy;
                int32_t rz = ((two_nv * Nz) >> 14) - vz;
                int32_t env[3];
                sample_cubemap_q12(rx, ry, rz, env);

                int32_t ao_q12 = ((int32_t)mrow[x].ao + 1) << 4;
                int32_t hemi_q12 = 2048 + (Ny >> 3);
                if (hemi_q12 < 0) hemi_q12 = 0; else if (hemi_q12 > 4096) hemi_q12 = 4096;
                int32_t ao_vis_q12 = 2048 + (ao_q12 >> 1);

                int32_t irr_r = ((((512 + ((544 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                 ((((4735 * ndotl0_q12 + 1024 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));
                int32_t irr_g = ((((480 + ((608 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                 ((((4481 * ndotl0_q12 + 1409 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));
                int32_t irr_b = ((((480 + ((768 * hemi_q12) >> 12)) * ao_q12) >> 12) +
                                 ((((4030 * ndotl0_q12 + 2114 * ndotl1_q12) >> 12) * ao_vis_q12) >> 12));

                int32_t env_ao_q12 = (env_s_q12 * ao_q12) >> 12;
                int32_t f5_q12 = (int32_t)s_fresnel_q12[ndotv_idx];
                int32_t fr_q12 = f0_r_q12 + (((4096 - f0_r_q12) * f5_q12) >> 12);
                int32_t fg_q12 = f0_g_q12 + (((4096 - f0_g_q12) * f5_q12) >> 12);
                int32_t fb_q12 = f0_b_q12 + (((4096 - f0_b_q12) * f5_q12) >> 12);

                int32_t mr = (((((4096 - fr_q12) * alb_diff_r_q12) >> 12) * irr_r +
                               fr_q12 * ((env[0] * env_ao_q12) >> 12)) >> 12) +
                             ((((spec0_q12 * 6554 + spec1_q12 * 1925) >> 12) * spec_tint_r_q12) >> 12);
                int32_t mg = (((((4096 - fg_q12) * alb_diff_g_q12) >> 12) * irr_g +
                               fg_q12 * ((env[1] * env_ao_q12) >> 12)) >> 12) +
                             ((((spec0_q12 * 6144 + spec1_q12 * 2499) >> 12) * spec_tint_g_q12) >> 12);
                int32_t mb = (((((4096 - fb_q12) * alb_diff_b_q12) >> 12) * irr_b +
                               fb_q12 * ((env[2] * env_ao_q12) >> 12)) >> 12) +
                             ((((spec0_q12 * 5571 + spec1_q12 * 3482) >> 12) * spec_tint_b_q12) >> 12);

                uint8_t aoi = grow[x].ao;
                uint8_t shv = shdrow_half ? shdrow_half[x >> 1] : 0u;
                int32_t vis_q12 = shv ? (4096 - (((int32_t)shv * 3020) >> 8)) : 4096;
                if (vis_q12 < 0) vis_q12 = 0;
                int32_t pr = (int32_t)s_ped_amb_q12[aoi][0] + (((int32_t)s_ped_dir_q12[aoi][0] * vis_q12) >> 12);
                int32_t pg = (int32_t)s_ped_amb_q12[aoi][1] + (((int32_t)s_ped_dir_q12[aoi][1] * vis_q12) >> 12);
                int32_t pb = (int32_t)s_ped_amb_q12[aoi][2] + (((int32_t)s_ped_dir_q12[aoi][2] * vis_q12) >> 12);
                drow[x] = tp_q12_to_xrgb32((pr * 2539 + mr * 1638) >> 12,
                                           (pg * 2539 + mg * 1638) >> 12,
                                           (pb * 2539 + mb * 1638) >> 12);
            }
        }
    }
    return true;
}

typedef struct {
    int w, x0, x1, y1, ox, oy, tw, th;
    const uint32_t *src_tile;
    const uint8_t  *lum_tile;
    uint32_t *fb;
    volatile int next_y;
} aa32_job_t;

static void edge_aa32_worker(int tid, void *ctx) {
    (void)tid;
    aa32_job_t *j = (aa32_job_t *)ctx;
    const int w = j->w, x0 = j->x0, x1 = j->x1, y1 = j->y1;
    const int ox = j->ox, oy = j->oy, tw = j->tw, th = j->th;
    const uint32_t *aa_src = j->src_tile;
    const uint8_t  *aa_lum = j->lum_tile;
    uint32_t *fb = j->fb;

    for (;;) {
        int ys = __atomic_fetch_add(&j->next_y, 4, __ATOMIC_RELAXED);
        if (ys > y1) break;
        int ye = ys + 3;
        if (ye > y1) ye = y1;

        for (int y = ys; y <= ye; y++) {
            int ty = y - oy;
            size_t f_roff = (size_t)y * (size_t)w;
            size_t t_roff = (size_t)ty * (size_t)tw;
            const gbuf_px_t *g_up = &s_gbuf[f_roff - (size_t)w];
            const gbuf_px_t *g_c  = &s_gbuf[f_roff];
            const gbuf_px_t *g_dn = &s_gbuf[f_roff + (size_t)w];
            const uint16_t  *z_up = s_zbuf ? &s_zbuf[f_roff - (size_t)w] : NULL;
            const uint16_t  *z_c  = s_zbuf ? &s_zbuf[f_roff]             : NULL;
            const uint16_t  *z_dn = s_zbuf ? &s_zbuf[f_roff + (size_t)w] : NULL;
            uint32_t        *dst  = &fb[f_roff];

            for (int x = x0; x <= x1; x++) {
                int tx = x - ox;
                size_t ti = t_roff + (size_t)tx;
                size_t tiN = ti - (size_t)tw, tiS = ti + (size_t)tw;
                size_t tiW = ti - 1u,         tiE = ti + 1u;

                int lM = (int)aa_lum[ti];
                int lN = (int)aa_lum[tiN], lS = (int)aa_lum[tiS];
                int lW = (int)aa_lum[tiW], lE = (int)aa_lum[tiE];
                int lMax = lM; if (lN > lMax) lMax = lN; if (lS > lMax) lMax = lS; if (lW > lMax) lMax = lW; if (lE > lMax) lMax = lE;
                int lMin = lM; if (lN < lMin) lMin = lN; if (lS < lMin) lMin = lS; if (lW < lMin) lMin = lW; if (lE < lMin) lMin = lE;
                int lRange = lMax - lMin;

                uint8_t mc = g_c[x].mat;
                bool mat_edge = (mc != g_c[x - 1].mat || mc != g_c[x + 1].mat || mc != g_up[x].mat || mc != g_dn[x].mat);
                bool z_edge = false;
                if (z_c && !mat_edge) {
                    int zc = (int)z_c[x];
                    int dzN = abs(zc - (int)z_up[x]), dzS = abs(zc - (int)z_dn[x]);
                    int dzW = abs(zc - (int)z_c[x - 1]), dzE = abs(zc - (int)z_c[x + 1]);
                    int dzM = dzN; if (dzS > dzM) dzM = dzS; if (dzW > dzM) dzM = dzW; if (dzE > dzM) dzM = dzE;
                    z_edge = (dzM > 900);
                }
                int lThr = (lMax * 24) >> 8;
                if (lThr < 9) lThr = 9;
                if (!mat_edge && !z_edge && lRange < lThr) continue;

                int lNW = (int)aa_lum[tiN - 1u], lNE = (int)aa_lum[tiN + 1u];
                int lSW = (int)aa_lum[tiS - 1u], lSE = (int)aa_lum[tiS + 1u];
                int lCross = (lN + lS + lW + lE) >> 2;
                float subpix = (lRange > 0) ? ((float)abs(lCross - lM) / (float)lRange) : 0.42f;
                if (subpix > 1.0f) subpix = 1.0f;
                subpix = subpix * subpix * (3.0f - 2.0f * subpix) * (mat_edge ? 0.62f : 0.50f);

                int edgeH = abs(-2 * lW + lNW + lSW) + 2 * abs(-2 * lM + lN + lS) + abs(-2 * lE + lNE + lSE);
                int edgeV = abs(-2 * lN + lNW + lNE) + 2 * abs(-2 * lM + lW + lE) + abs(-2 * lS + lSW + lSE);
                bool horz = (edgeH >= edgeV);

                int lNeg = horz ? lN : lW, lPos = horz ? lS : lE;
                int gNeg = abs(lNeg - lM), gPos = abs(lPos - lM);
                bool useNeg = (gNeg >= gPos);
                size_t iOther = horz ? (useNeg ? tiN : tiS) : (useNeg ? tiW : tiE);
                int lOther = useNeg ? lNeg : lPos;
                int lAvg2 = lM + lOther;
                int gTol2 = ((gNeg > gPos ? gNeg : gPos) >> 1);
                if (gTol2 < 8) gTol2 = 8;

                int dxt = horz ? 1 : 0, dyt = horz ? 0 : 1;
                int pd = useNeg ? -1 : 1;
                int dn = 5, dp = 5;
                int endN2 = lM << 1, endP2 = lM << 1;
                for (int s = 1; s <= 5; s++) {
                    int stx = tx - dxt * s, sty = ty - dyt * s;
                    if (stx < 0 || sty < 0) { dn = s; break; }
                    int px2 = horz ? stx : ((stx + pd < 0 || stx + pd >= tw) ? stx : stx + pd);
                    int py2 = horz ? ((sty + pd < 0 || sty + pd >= th) ? sty : sty + pd) : sty;
                    int ln2 = (int)aa_lum[(size_t)sty * (size_t)tw + (size_t)stx] + (int)aa_lum[(size_t)py2 * (size_t)tw + (size_t)px2];
                    endN2 = ln2;
                    if (abs(ln2 - lAvg2) >= gTol2) { dn = s; break; }
                }
                for (int s = 1; s <= 5; s++) {
                    int stx = tx + dxt * s, sty = ty + dyt * s;
                    if (stx >= tw || sty >= th) { dp = s; break; }
                    int px2 = horz ? stx : ((stx + pd < 0 || stx + pd >= tw) ? stx : stx + pd);
                    int py2 = horz ? ((sty + pd < 0 || sty + pd >= th) ? sty : sty + pd) : sty;
                    int lp2 = (int)aa_lum[(size_t)sty * (size_t)tw + (size_t)stx] + (int)aa_lum[(size_t)py2 * (size_t)tw + (size_t)px2];
                    endP2 = lp2;
                    if (abs(lp2 - lAvg2) >= gTol2) { dp = s; break; }
                }

                float edgeBlend = 0.0f;
                int dNear = (dn < dp) ? dn : dp;
                int endNear2 = (dn < dp) ? endN2 : endP2;
                if (((endNear2 - lAvg2) < 0) != (((lM << 1) - lAvg2) < 0)) {
                    edgeBlend = 0.5f - (float)dNear / (float)(dn + dp);
                    if (edgeBlend < 0.0f) edgeBlend = 0.0f;
                }
                float blend = (subpix > edgeBlend) ? subpix : edgeBlend;
                if (blend > 0.72f) blend = 0.72f;
                int bw = (int)(blend * 256.0f + 0.5f);
                if (bw > 1) {
                    int omw = 256 - bw;
                    uint32_t c0 = aa_src[ti], c1 = aa_src[iOther];
                    uint32_t r = ((((c0 >> 16) & 0xFFu) * (uint32_t)omw + ((c1 >> 16) & 0xFFu) * (uint32_t)bw) + 128u) >> 8;
                    uint32_t g = ((((c0 >>  8) & 0xFFu) * (uint32_t)omw + ((c1 >>  8) & 0xFFu) * (uint32_t)bw) + 128u) >> 8;
                    uint32_t b = (((c0 & 0xFFu) * (uint32_t)omw + (c1 & 0xFFu) * (uint32_t)bw) + 128u) >> 8;
                    dst[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
        }
    }
}

static void edge_aa_xrgb32(int w, int h, uint32_t *fb) {
    if (!fb || !s_gbuf || w < 6 || h < 6) return;
    int x0 = (s_dirty_x0 - 1 > 1) ? (s_dirty_x0 - 1) : 1;
    int y0 = (s_dirty_y0 - 1 > 1) ? (s_dirty_y0 - 1) : 1;
    int x1 = (s_dirty_x1 + 1 < w - 2) ? (s_dirty_x1 + 1) : (w - 2);
    int y1 = (s_dirty_y1 + 1 < h - 2) ? (s_dirty_y1 + 1) : (h - 2);
    if (x1 < x0 || y1 < y0) return;

    int tw = (x1 - x0 + 1) + 2;
    int th = (y1 - y0 + 1) + 2;
    size_t tile_n = (size_t)tw * (size_t)th;
    static uint32_t *s_aa32_src = NULL;
    static uint8_t  *s_aa32_lum = NULL;
    static size_t    s_aa32_cap = 0u;
    if (tile_n > s_aa32_cap) {
        free(s_aa32_src); free(s_aa32_lum);
        s_aa32_src = (uint32_t *)malloc(tile_n * sizeof(uint32_t));
        s_aa32_lum = (uint8_t *)malloc(tile_n);
        if (!s_aa32_src || !s_aa32_lum) {
            free(s_aa32_src); free(s_aa32_lum);
            s_aa32_src = NULL; s_aa32_lum = NULL; s_aa32_cap = 0u;
            return;
        }
        s_aa32_cap = tile_n;
    }

    int ox = x0 - 1, oy = y0 - 1;
    for (int ty = 0; ty < th; ty++) {
        size_t srow = (size_t)(oy + ty) * (size_t)w + (size_t)ox;
        size_t drow = (size_t)ty * (size_t)tw;
        memcpy(&s_aa32_src[drow], &fb[srow], (size_t)tw * sizeof(uint32_t));
        for (int tx = 0; tx < tw; tx++) {
            uint32_t c = s_aa32_src[drow + (size_t)tx];
            s_aa32_lum[drow + (size_t)tx] = (uint8_t)((54u * ((c >> 16) & 0xFFu) + 183u * ((c >> 8) & 0xFFu) + 19u * (c & 0xFFu)) >> 8);
        }
    }

    aa32_job_t aj = {
        .w = w, .x0 = x0, .x1 = x1, .y1 = y1,
        .ox = ox, .oy = oy, .tw = tw, .th = th,
        .src_tile = s_aa32_src,
        .lum_tile = s_aa32_lum,
        .fb = fb,
        .next_y = y0
    };
    tp_dispatch(edge_aa32_worker, &aj);
}

static inline uint8_t tp_aces_srgb8_scalar(double v) {
    if (!(v > 0.0)) return 0u;
    int32_t q8 = (int32_t)(v * 256.0 + 0.5);
    if (q8 < 0) q8 = 0; else if (q8 >= HDR_Q8_MAX) q8 = HDR_Q8_MAX - 1;
    return s_aces_lut[q8];
}

static void draw_teapot_hud_64f(ipo_rgba64f_t *fb, int w, int h, const char *text);

static void draw_teapot_hud_xrgb32(uint32_t *fb32, int w, int h, int gl_h, const char *text) {
    if (!fb32 || !s_hdr_fb || !text || h < 20 || w < 40) return;
    draw_teapot_hud_64f(s_hdr_fb, w, h, text);
    int y0 = (gl_h > 0 && gl_h < h) ? gl_h : (h - 14);
    for (int y = y0; y < h; y++) {
        size_t off = (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            const ipo_rgba64f_t *p = &s_hdr_fb[off + (size_t)x];
            fb32[off + (size_t)x] = 0xFF000000u |
                ((uint32_t)tp_aces_srgb8_scalar(p->r) << 16) |
                ((uint32_t)tp_aces_srgb8_scalar(p->g) << 8) |
                (uint32_t)tp_aces_srgb8_scalar(p->b);
        }
    }
}

static void draw_teapot_cursor_xrgb32(uint32_t *fb, int w, int h,
                                      int cx, int cy, bool active) {
    if (!fb || cx < 0 || cx >= w || cy < 0 || cy >= h) return;
    uint32_t rim = 0xFF1A1A22u;
    uint32_t col = active ? 0xFFFFDB40u : 0xFF70EEFFu;
    static const int offs[] = { -7, -6, -5, -4, -3, 3, 4, 5, 6, 7 };
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        int d = offs[i];
        if (cx + d >= 0 && cx + d < w) {
            if (cy - 1 >= 0) fb[(size_t)(cy - 1) * (size_t)w + (cx + d)] = rim;
            if (cy + 1 < h)  fb[(size_t)(cy + 1) * (size_t)w + (cx + d)] = rim;
            fb[(size_t)cy * (size_t)w + (cx + d)] = col;
        }
        if (cy + d >= 0 && cy + d < h) {
            if (cx - 1 >= 0) fb[(size_t)(cy + d) * (size_t)w + (cx - 1)] = rim;
            if (cx + 1 < w)  fb[(size_t)(cy + d) * (size_t)w + (cx + 1)] = rim;
            fb[(size_t)(cy + d) * (size_t)w + cx] = col;
        }
    }
    fb[(size_t)cy * (size_t)w + cx] = 0xFFFFFFFFu;
}

static void draw_teapot_hud_64f(ipo_rgba64f_t *fb, int w, int h, const char *text) {
    if (!fb || !text || h < 20 || w < 40) return;
    int bar_h = 14;
    int bar_y = h - bar_h;
    ipo_gfx3d_ctx_t tmp_ctx;
    memset(&tmp_ctx, 0, sizeof(tmp_ctx));
    tmp_ctx.width = w;
    tmp_ctx.height = h;
    tmp_ctx.color_fb = fb;
    ipo_rgba64f_t bg = { 0.025, 0.030, 0.042, 1.0 };
    ipo_gfx3d_draw_rect2d(&tmp_ctx, 0, bar_y, w, bar_h, bg);
    ipo_rgba64f_t rule = { 0.18, 0.22, 0.30, 0.90 };
    ipo_gfx3d_draw_rect2d(&tmp_ctx, 0, bar_y, w, 1, rule);
    ipo_rgba64f_t fg = { 0.92, 0.94, 0.98, 1.0 };
    ipo_gfx3d_draw_text2d(&tmp_ctx, 6, bar_y + 4, text, fg, 1);
}

static void draw_teapot_cursor_64f(ipo_rgba64f_t *fb, int w, int h,
                                   int cx, int cy, bool active) {
    if (!fb || cx < 0 || cx >= w || cy < 0 || cy >= h) return;
    ipo_rgba64f_t rim = { 0.02, 0.02, 0.03, 1.0 };
    ipo_rgba64f_t col = active ? (ipo_rgba64f_t){ 1.4, 1.0, 0.15, 1.0 }
                               : (ipo_rgba64f_t){ 0.35, 1.05, 1.25, 1.0 };
    static const int offs[] = { -7, -6, -5, -4, -3, 3, 4, 5, 6, 7 };
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        int d = offs[i];
        if (cx + d >= 0 && cx + d < w) {
            if (cy - 1 >= 0) fb[(size_t)(cy - 1) * (size_t)w + (cx + d)] = rim;
            if (cy + 1 < h)  fb[(size_t)(cy + 1) * (size_t)w + (cx + d)] = rim;
            fb[(size_t)cy * (size_t)w + (cx + d)] = col;
        }
        if (cy + d >= 0 && cy + d < h) {
            if (cx - 1 >= 0) fb[(size_t)(cy + d) * (size_t)w + (cx - 1)] = rim;
            if (cx + 1 < w)  fb[(size_t)(cy + d) * (size_t)w + (cx + 1)] = rim;
            fb[(size_t)(cy + d) * (size_t)w + cx] = col;
        }
    }
    fb[(size_t)cy * (size_t)w + cx] = (ipo_rgba64f_t){ 1.5, 1.5, 1.5, 1.0 };
}

/* =========================================================================
 * Custom Viewport Cursor & Window/Fullscreen Size Logic
 * ========================================================================= */
static void draw_teapot_cursor(uint8_t *buf, uint16_t bw, uint16_t bh,
                               int cx, int cy, bool active) {
    if (!buf) return;
    uint8_t accent = active ? 14u : 11u;
    static const int arm_offsets[] = { -5, -4, -3, -2, 2, 3, 4, 5 };
    for (size_t i = 0; i < sizeof(arm_offsets) / sizeof(arm_offsets[0]); i++) {
        int d = arm_offsets[i];
        wm_buf_put_pixel(buf, bw, bh, cx + d, cy - 1, 0u);
        wm_buf_put_pixel(buf, bw, bh, cx + d, cy + 1, 0u);
        wm_buf_put_pixel(buf, bw, bh, cx + d, cy, accent);

        wm_buf_put_pixel(buf, bw, bh, cx - 1, cy + d, 0u);
        wm_buf_put_pixel(buf, bw, bh, cx + 1, cy + d, 0u);
        wm_buf_put_pixel(buf, bw, bh, cx, cy + d, accent);
    }
    wm_buf_put_pixel(buf, bw, bh, cx - 6, cy, 0u);
    wm_buf_put_pixel(buf, bw, bh, cx + 6, cy, 0u);
    wm_buf_put_pixel(buf, bw, bh, cx, cy - 6, 0u);
    wm_buf_put_pixel(buf, bw, bh, cx, cy + 6, 0u);
    wm_buf_put_pixel(buf, bw, bh, cx, cy, 15u);
}

static void refresh_gfx_caps(void) {
    if (ipo_gfx_get_caps(&s_gfx_caps) == 0) {
        s_caps_ready = true;
    }
}

static void target_render_size(wm_window_t *win, int *out_w, int *out_h, bool *out_hd_fs) {
    if (!s_caps_ready) refresh_gfx_caps();

    if (s_caps_ready && s_gfx_caps.active_hw_bpp >= 15u) {
        ipo_gpu_info_t gi;
        bool has_virgl = (ipo_gpu_get_info(&gi) &&
                          gi.backend_mode == IPO_GPU_BACKEND_VIRGL_3D &&
                          !gi.forced_cpu_fallback);
        if (win && win->fullscreen) {
            int rw = (int)s_gfx_caps.active_width;
            int rh = (int)s_gfx_caps.active_height;
            if (!has_virgl && rw > 720) {
                rh = (rh * 720) / rw;
                rw = 720;
            }
            *out_w = (rw > 32) ? rw : DEFAULT_WIN_W;
            *out_h = (rh > 32) ? rh : DEFAULT_WIN_H;
            *out_hd_fs = true;
            return;
        }
        int px = 0, py = 0, pw = 0, ph = 0;
        ipo_gfx_wm_window_phys_rect(win, &px, &py, &pw, &ph);
        int rw = (pw > 32) ? pw : DEFAULT_WIN_W;
        int rh = (ph > 32) ? ph : DEFAULT_WIN_H;
        if (!has_virgl && rw > 720) {
            rh = (rh * 720) / rw;
            rw = 720;
        }
        *out_w = (rw > 32) ? rw : DEFAULT_WIN_W;
        *out_h = (rh > 32) ? rh : DEFAULT_WIN_H;
        *out_hd_fs = false;
        return;
    }

    int bga_w = s_caps_ready ? (int)s_gfx_caps.active_width : 320;
    int bga_h = s_caps_ready ? (int)s_gfx_caps.active_height : 200;
    if (win && win->fullscreen && bga_w >= 640 && bga_h >= 400) {
        *out_w = bga_w / 2;
        *out_h = bga_h / 2;
        *out_hd_fs = true;
    } else {
        int ww = (win && win->w > 0) ? (int)win->w : DEFAULT_WIN_W;
        int wh = (win && win->h > 0) ? (int)win->h : DEFAULT_WIN_H;
        *out_w = ww;
        *out_h = wh;
        *out_hd_fs = false;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    uint32_t pid = (uint32_t)ipo_syscall(IPO_SYSCALL_GETPID, 0, NULL);
    if (pid == 0) pid = 1;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Realistic Studio Utah Teapot";
    opt.x = 20 + ((pid * 22) % 60);
    opt.y = 14 + ((pid * 14) % 30);
    opt.w = DEFAULT_WIN_W;
    opt.h = DEFAULT_WIN_H;
    opt.decor_style = WM_DECOR_DEFAULT;

    (void)ipo_gfx_ensure_hd_mode(1280u, 720u);
    wm_window_t *win = wm_create_window(&opt);
    if (!win) return 1;
    wm_set_cursor_visible(win, false);

    int ren_w = 0, ren_h = 0;
    bool hd_fs = false;
    refresh_gfx_caps();
    tp_init();
    bool use_rgba64f = (s_caps_ready && s_gfx_caps.active_hw_bpp >= 15u);
    target_render_size(win, &ren_w, &ren_h, &hd_fs);
    int gl_h = use_rgba64f ? ((ren_h > 24) ? (ren_h - 14) : ren_h)
                           : ((ren_h > 20) ? (ren_h - 12) : ren_h);

    uint16_t last_win_w = win->w;
    uint16_t last_win_h = win->h;
    bool     last_win_fs = win->fullscreen;

    GLContext *gl = gl_create_context(ren_w, gl_h, win);
    if (!gl) {
        wm_destroy_window(win);
        return 1;
    }
    gl_make_current(gl);

    ipo_hd_view_t hd;
    memset(&hd, 0, sizeof(hd));
    hd.prefer_int_scale = true; /* Exact 2x2 pixel doubling in 800x600 HD-FS */

    uint8_t *fs_fb8 = NULL;
    size_t fs_fb8_cap = 0;

    const float light0_dir[4] = { 0.55f, 0.78f, 0.44f, 0.0f };
    const float ground_plane[4] = { 0.0f, 1.0f, 0.0f, 0.910f };
    float shadow_mat[16];
    build_shadow_matrix(shadow_mat, light0_dir, ground_plane);

    bool mouse_dragging = false;
    int last_mx = 0, last_my = 0;

    uint32_t prev_tick = timer_millis();
    uint32_t fps_timer = prev_tick;
    uint32_t fps_frame_count = 0;
    int current_fps = 0;
    char *prev_status_buf = NULL;
    int prev_cur_x0 = 0, prev_cur_y0 = 0, prev_cur_x1 = -1, prev_cur_y1 = -1;

    while (app_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        wm_compositor_tick();
        wm_event_t ev;
        while (wm_poll_event(win, &ev)) {
            on_window_event(win, ev.type, ev.data);
        }
        if (!app_running || !wm_is_window_valid(win)) break;

        if (win->w != last_win_w || win->h != last_win_h || win->fullscreen != last_win_fs) {
            last_win_w = win->w;
            last_win_h = win->h;
            last_win_fs = win->fullscreen;
            refresh_gfx_caps();
            use_rgba64f = (s_caps_ready && s_gfx_caps.active_hw_bpp >= 15u);
            int next_w = 0, next_h = 0;
            bool next_hd_fs = false;
            target_render_size(win, &next_w, &next_h, &next_hd_fs);
            if (next_w != ren_w || next_h != ren_h || next_hd_fs != hd_fs) {
                ren_w = next_w;
                ren_h = next_h;
                hd_fs = next_hd_fs;
                gl_h = use_rgba64f ? ((ren_h > 24) ? (ren_h - 14) : ren_h)
                                   : ((ren_h > 20) ? (ren_h - 12) : ren_h);
                gl_resize_context(gl, ren_w, gl_h);
                glViewport(0, 0, ren_w, gl_h);
                s_hdr_bg_primed = false;
                if (prev_status_buf) prev_status_buf[0] = '\0';
                prev_cur_x1 = -1;
                if (!use_rgba64f && !hd_fs && win->framebuf) {
                    memset(win->framebuf, 0, (size_t)win->w * (size_t)win->h);
                }
            }
        }

        if (wm_get_focused() == win) {
            mouse_state_t ms;
            mouse_get_state(&ms);
            if (ms.left_button) {
                if (!mouse_dragging) {
                    if (ms.x >= (int)win->x && ms.x < (int)win->x + (int)win->w &&
                        ms.y >= (int)win->y && ms.y < (int)win->y + (int)win->h) {
                        mouse_dragging = true;
                        last_mx = ms.x;
                        last_my = ms.y;
                    }
                } else {
                    int dx = ms.x - last_mx;
                    int dy = ms.y - last_my;
                    if (dx != 0 || dy != 0) {
                        rot_y += (float)dx * 1.4f;
                        rot_x += (float)dy * 1.4f;
                        if (rot_x < -40.0f) rot_x = -40.0f;
                        if (rot_x > 75.0f)  rot_x = 75.0f;
                        auto_spin = false;
                        last_mx = ms.x;
                        last_my = ms.y;
                    }
                }
            } else {
                mouse_dragging = false;
            }
        } else {
            mouse_dragging = false;
        }

        uint32_t frame_now = timer_millis();
        uint32_t dt_ms = frame_now - prev_tick;
        prev_tick = frame_now;
        if (dt_ms > 100u) dt_ms = 100u;

        if (auto_spin) {
            rot_y += 28.0f * ((float)dt_ms * 0.001f);
            if (rot_y >= 360.0f) rot_y -= 360.0f;
        }

        gl_make_current(gl);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        double aspect = (gl_h > 0) ? ((double)ren_w / (double)gl_h) : 1.333;
        gluPerspective(42.0, aspect, 1.0, 40.0);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef(0.0f, -0.02f, -cam_dist);
        glRotatef(rot_x, 1.0f, 0.0f, 0.0f);

        int frame_mat = current_mat;
        uint32_t t0 = timer_millis();
        uint32_t t_sh0 = t0, t_pr0 = t0;
        bool used_gpu = false;
        if (render_scene_gpu(win, ren_w, gl_h, ren_h, frame_mat, current_fps, hd_fs, mouse_dragging)) {
            used_gpu = true;
            t_sh0 = t0 + s_tp_gpu.last_assets_ms;
            t_pr0 = t_sh0 + s_tp_gpu.last_flush_ms;
        } else {
        __atomic_store_n(&s_tp_hot, 1, __ATOMIC_RELEASE);
        bool has_soft_shd = render_scene_gbuffer(ren_w, gl_h, ren_h, rot_y, shadow_mat, use_rgba64f, frame_mat);

        if (use_rgba64f && s_hdr_fb) {
            t_sh0 = timer_millis();
            bool use_xrgb32 = shade_deferred_pbr_xrgb32(ren_w, gl_h, ren_h, frame_mat, has_soft_shd);
            if (use_xrgb32 && s_xrgb_fb) {
                edge_aa_xrgb32(ren_w, gl_h, s_xrgb_fb);
                __atomic_store_n(&s_tp_hot, 0, __ATOMIC_RELEASE);
                t_pr0 = timer_millis();
                bool hud_dirty = false;
                if (ren_h > gl_h) {
                    char *status_buf = tp_alloc_fmt(
                        "%d FPS | %s | PBR32 HDR | %dx%d | M:Mat S:%s L:%s F11:%s",
                        current_fps, materials[frame_mat].name, ren_w, ren_h,
                        shadows_enabled ? "On" : "Off",
                        light1_enabled ? "2L" : "1L",
                        hd_fs ? "Win" : "FS");
                    if (status_buf) {
                        if (s_dirty_full_frame || !prev_status_buf || strcmp(status_buf, prev_status_buf) != 0) {
                            draw_teapot_hud_xrgb32(s_xrgb_fb, ren_w, ren_h, gl_h, status_buf);
                            free(prev_status_buf);
                            prev_status_buf = status_buf;
                            hud_dirty = true;
                        } else {
                            free(status_buf);
                        }
                    }
                }
                uint32_t cur_save32[15 * 15];
                int c_x0 = 0, c_y0 = 0, c_x1 = -1, c_y1 = -1;
                if (wm_get_focused() == win) {
                    mouse_state_t cur_ms;
                    mouse_get_state(&cur_ms);
                    int lx = (int)cur_ms.x - (int)win->x;
                    int ly = (int)cur_ms.y - (int)win->y;
                    if (lx >= 0 && lx < (int)win->w && ly >= 0 && ly < (int)win->h) {
                        int px = (lx * ren_w) / (win->w > 0 ? (int)win->w : 1);
                        int py = (ly * ren_h) / (win->h > 0 ? (int)win->h : 1);
                        c_x0 = px - 7; if (c_x0 < 0) c_x0 = 0;
                        c_y0 = py - 7; if (c_y0 < 0) c_y0 = 0;
                        c_x1 = px + 7; if (c_x1 >= ren_w) c_x1 = ren_w - 1;
                        c_y1 = py + 7; if (c_y1 >= ren_h) c_y1 = ren_h - 1;
                        for (int cy = c_y0; cy <= c_y1; cy++) {
                            for (int cx = c_x0; cx <= c_x1; cx++) {
                                cur_save32[(cy - c_y0) * 15 + (cx - c_x0)] = s_xrgb_fb[(size_t)cy * (size_t)ren_w + (size_t)cx];
                            }
                        }
                        draw_teapot_cursor_xrgb32(s_xrgb_fb, ren_w, ren_h, px, py, mouse_dragging);
                    }
                }
                ipo_gfx_surface_desc_t surf;
                memset(&surf, 0, sizeof(surf));
                surf.struct_size = (uint32_t)sizeof(surf);
                surf.width = (uint32_t)ren_w;
                surf.height = (uint32_t)ren_h;
                surf.stride_bytes = (uint32_t)ren_w * (uint32_t)sizeof(uint32_t);
                surf.format_id = IPO_COLOR_FMT_XRGB8888;
                surf.tonemap_op = IPO_TONEMAP_NONE;
                surf.exposure = 1.0;
                surf.pixels = s_xrgb_fb;
                if (!s_dirty_full_frame && !hud_dirty && s_dirty_x1 >= s_dirty_x0 && s_dirty_y1 >= s_dirty_y0) {
                    int dx0 = s_dirty_x0, dy0 = s_dirty_y0, dx1 = s_dirty_x1, dy1 = s_dirty_y1;
                    if (prev_cur_x1 >= prev_cur_x0) {
                        if (prev_cur_x0 < dx0) dx0 = prev_cur_x0;
                        if (prev_cur_y0 < dy0) dy0 = prev_cur_y0;
                        if (prev_cur_x1 > dx1) dx1 = prev_cur_x1;
                        if (prev_cur_y1 > dy1) dy1 = prev_cur_y1;
                    }
                    if (c_x1 >= c_x0) {
                        if (c_x0 < dx0) dx0 = c_x0;
                        if (c_y0 < dy0) dy0 = c_y0;
                        if (c_x1 > dx1) dx1 = c_x1;
                        if (c_y1 > dy1) dy1 = c_y1;
                    }
                    surf.dst_x = dx0;
                    surf.dst_y = dy0;
                    surf.dst_w = dx1 - dx0 + 1;
                    surf.dst_h = dy1 - dy0 + 1;
                }
                ipo_gfx_present_window(win, &surf);
                if (c_x1 >= c_x0) {
                    for (int cy = c_y0; cy <= c_y1; cy++) {
                        for (int cx = c_x0; cx <= c_x1; cx++) {
                            s_xrgb_fb[(size_t)cy * (size_t)ren_w + (size_t)cx] = cur_save32[(cy - c_y0) * 15 + (cx - c_x0)];
                        }
                    }
                }
                prev_cur_x0 = c_x0; prev_cur_y0 = c_y0; prev_cur_x1 = c_x1; prev_cur_y1 = c_y1;
            } else {
            shade_deferred_pbr_rgba64f(ren_w, gl_h, s_hdr_fb, frame_mat, has_soft_shd);
            edge_aa_hdr64f(ren_w, gl_h, s_hdr_fb);
            __atomic_store_n(&s_tp_hot, 0, __ATOMIC_RELEASE);
            t_pr0 = timer_millis();
            bool hud_dirty = false;
            if (ren_h > gl_h) {
                char *status_buf = tp_alloc_fmt(
                    "%d FPS | %s | RGBA64F 256bpp | %dx%d | M:Mat S:%s L:%s F11:%s",
                    current_fps, materials[frame_mat].name, ren_w, ren_h,
                    shadows_enabled ? "On" : "Off",
                    light1_enabled ? "2L" : "1L",
                    hd_fs ? "Win" : "FS");
                if (status_buf) {
                    if (s_dirty_full_frame || !prev_status_buf || strcmp(status_buf, prev_status_buf) != 0) {
                        draw_teapot_hud_64f(s_hdr_fb, ren_w, ren_h, status_buf);
                        free(prev_status_buf);
                        prev_status_buf = status_buf;
                        hud_dirty = true;
                    } else {
                        free(status_buf);
                    }
                }
            }
            ipo_rgba64f_t cur_save[15 * 15];
            int c_x0 = 0, c_y0 = 0, c_x1 = -1, c_y1 = -1;
            if (wm_get_focused() == win) {
                mouse_state_t cur_ms;
                mouse_get_state(&cur_ms);
                int lx = (int)cur_ms.x - (int)win->x;
                int ly = (int)cur_ms.y - (int)win->y;
                if (lx >= 0 && lx < (int)win->w && ly >= 0 && ly < (int)win->h) {
                    int px = (lx * ren_w) / (win->w > 0 ? (int)win->w : 1);
                    int py = (ly * ren_h) / (win->h > 0 ? (int)win->h : 1);
                    c_x0 = px - 7; if (c_x0 < 0) c_x0 = 0;
                    c_y0 = py - 7; if (c_y0 < 0) c_y0 = 0;
                    c_x1 = px + 7; if (c_x1 >= ren_w) c_x1 = ren_w - 1;
                    c_y1 = py + 7; if (c_y1 >= ren_h) c_y1 = ren_h - 1;
                    for (int cy = c_y0; cy <= c_y1; cy++) {
                        for (int cx = c_x0; cx <= c_x1; cx++) {
                            cur_save[(cy - c_y0) * 15 + (cx - c_x0)] = s_hdr_fb[(size_t)cy * (size_t)ren_w + (size_t)cx];
                        }
                    }
                    draw_teapot_cursor_64f(s_hdr_fb, ren_w, ren_h, px, py, mouse_dragging);
                }
            }
            ipo_gfx_surface_desc_t surf;
            memset(&surf, 0, sizeof(surf));
            surf.struct_size = (uint32_t)sizeof(surf);
            surf.width = (uint32_t)ren_w;
            surf.height = (uint32_t)ren_h;
            surf.stride_bytes = (uint32_t)ren_w * (uint32_t)sizeof(ipo_rgba64f_t);
            surf.format_id = IPO_COLOR_FMT_RGBA64F;
            surf.tonemap_op = IPO_TONEMAP_ACES;
            surf.exposure = 1.0;
            surf.pixels = s_hdr_fb;
            if (!s_dirty_full_frame && !hud_dirty && s_dirty_x1 >= s_dirty_x0 && s_dirty_y1 >= s_dirty_y0) {
                int dx0 = s_dirty_x0, dy0 = s_dirty_y0, dx1 = s_dirty_x1, dy1 = s_dirty_y1;
                if (prev_cur_x1 >= prev_cur_x0) {
                    if (prev_cur_x0 < dx0) dx0 = prev_cur_x0;
                    if (prev_cur_y0 < dy0) dy0 = prev_cur_y0;
                    if (prev_cur_x1 > dx1) dx1 = prev_cur_x1;
                    if (prev_cur_y1 > dy1) dy1 = prev_cur_y1;
                }
                if (c_x1 >= c_x0) {
                    if (c_x0 < dx0) dx0 = c_x0;
                    if (c_y0 < dy0) dy0 = c_y0;
                    if (c_x1 > dx1) dx1 = c_x1;
                    if (c_y1 > dy1) dy1 = c_y1;
                }
                surf.dst_x = dx0;
                surf.dst_y = dy0;
                surf.dst_w = dx1 - dx0 + 1;
                surf.dst_h = dy1 - dy0 + 1;
            }
            ipo_gfx_present_window(win, &surf);
            if (c_x1 >= c_x0) {
                for (int cy = c_y0; cy <= c_y1; cy++) {
                    for (int cx = c_x0; cx <= c_x1; cx++) {
                        s_hdr_fb[(size_t)cy * (size_t)ren_w + (size_t)cx] = cur_save[(cy - c_y0) * 15 + (cx - c_x0)];
                    }
                }
            }
            prev_cur_x0 = c_x0; prev_cur_y0 = c_y0; prev_cur_x1 = c_x1; prev_cur_y1 = c_y1;
            }
        } else if (hd_fs && wm_get_focused() == win) {
            __atomic_store_n(&s_tp_hot, 0, __ATOMIC_RELEASE);
            t_sh0 = timer_millis();
            size_t need = (size_t)ren_w * (size_t)ren_h;
            if (need > fs_fb8_cap) {
                uint8_t *nf = (uint8_t *)realloc(fs_fb8, need);
                if (nf) { fs_fb8 = nf; fs_fb8_cap = need; }
            }
            if (fs_fb8) {
                shade_deferred_pbr_to_wm8(ren_w, gl_h, fs_fb8, ren_w, frame_mat, has_soft_shd);
                if (ren_h > gl_h) {
                    int bar_y = gl_h;
                    int bar_h = ren_h - gl_h;
                    wm_buf_fill_rect(fs_fb8, (uint16_t)ren_w, (uint16_t)ren_h, 0, bar_y, ren_w, bar_h, 17u);
                    char *status_buf = tp_alloc_fmt(
                        "%d FPS | %s | M:Mat S:%s L:%s F11:Win",
                        current_fps, materials[frame_mat].name,
                        shadows_enabled ? "Shd" : "NoShd",
                        light1_enabled ? "2L" : "1L");
                    if (status_buf) {
                        wm_buf_draw_string(fs_fb8, (uint16_t)ren_w, (uint16_t)ren_h, 6, bar_y + 2, status_buf, 31u);
                        free(status_buf);
                    }
                }
                t_pr0 = timer_millis();
                ipo_hd_present(&hd, win, false, fs_fb8, ren_w, ren_h, ren_w);
            }
        } else if (win->framebuf) {
            __atomic_store_n(&s_tp_hot, 0, __ATOMIC_RELEASE);
            t_sh0 = timer_millis();
            int pw = ren_w < (int)win->w ? ren_w : (int)win->w;
            int ph = gl_h < (int)win->h ? gl_h : (int)win->h;
            if (pw == ren_w && ph == gl_h) {
                shade_deferred_pbr_to_wm8(ren_w, gl_h, win->framebuf, win->w, frame_mat, has_soft_shd);
            }

            if ((int)win->h >= 40) {
                int bar_h = 12;
                int bar_y = (int)win->h - bar_h;
                wm_buf_fill_rect(win->framebuf, win->w, win->h, 0, bar_y, (int)win->w, bar_h, 17u);
                char *status_buf = tp_alloc_fmt(
                    "%dFPS|%s|M/S/L/F11",
                    current_fps, materials[frame_mat].name);
                if (status_buf) {
                    wm_buf_draw_string(win->framebuf, win->w, win->h, 4, bar_y + 2, status_buf, 31u);
                    free(status_buf);
                }
            }

            if (wm_get_focused() == win) {
                mouse_state_t cur_ms;
                mouse_get_state(&cur_ms);
                int local_x = (int)cur_ms.x - (int)win->x;
                int local_y = (int)cur_ms.y - (int)win->y;
                if (local_x >= 0 && local_x < (int)win->w &&
                    local_y >= 0 && local_y < (int)win->h) {
                    draw_teapot_cursor(win->framebuf, win->w, win->h,
                                       local_x, local_y, mouse_dragging);
                }
            }
            t_pr0 = timer_millis();
            wm_invalidate(win);
        }
        } /* end CPU fallback */
        uint32_t t1 = timer_millis();

        fps_frame_count++;
        uint32_t now = timer_millis();
        if (now - fps_timer >= 1000u) {
            current_fps = (int)(((float)fps_frame_count * 1000.0f) / (float)(now - fps_timer) + 0.5f);
            serial_printf("[gl_teapot] %d FPS (%dx%d %s %s) mat=%s frame=%ums (gb=%u sh=%u pr=%u)\n",
                          current_fps, ren_w, ren_h,
                          used_gpu ? "GPU-VirGL" : (use_rgba64f ? "RGBA64F" : "WM8"),
                          hd_fs ? "HD-FS" : "Win",
                          materials[frame_mat].name, (unsigned)(t1 - t0),
                          (unsigned)(t_sh0 - t0), (unsigned)(t_pr0 - t_sh0),
                          (unsigned)(t1 - t_pr0));
            fps_frame_count = 0;
            fps_timer = now;
            if (!hd_fs) {
                char *title_buf = tp_alloc_fmt("Utah Teapot PBR [%d FPS]", current_fps);
                if (title_buf) {
                    wm_set_title(win, title_buf);
                    free(title_buf);
                }
            }
        }
    }

    free(prev_status_buf);
    prev_status_buf = NULL;
    tp_shutdown();
    teapot_gpu_destroy();
    ipo_hd_free(&hd);
    free(fs_fb8);
    free(s_bg_idx_cache);
    s_bg_idx_cache = NULL;
    free(s_studio_bg_hdr);
    s_studio_bg_hdr = NULL;
    free(s_bg_hdr_cache);
    s_bg_hdr_cache = NULL;
    free(s_hdr_fb);
    s_hdr_fb = NULL;
    s_hdr_cap = 0;
    free(s_vx_col);
    s_vx_col = NULL;
    free(s_vy_row);
    s_vy_row = NULL;
    free(s_vx_col_q14);
    s_vx_col_q14 = NULL;
    free(s_vy_row_q14);
    s_vy_row_q14 = NULL;
    free(s_vdir_q14);
    s_vdir_q14 = NULL;
    free(s_teapot_ao);
    s_teapot_ao = NULL;
    free(s_teapot_normals);
    s_teapot_normals = NULL;
    free(s_sub_verts);
    s_sub_verts = NULL;
    free(s_sub_idx);
    s_sub_idx = NULL;
    s_sub_vcount = 0u;
    s_sub_tcount = 0u;
    free(s_tverts);
    s_tverts = NULL;
    free(s_mverts);
    s_mverts = NULL;
    free(s_sverts);
    s_sverts = NULL;
    free(s_vis_tris);
    s_vis_tris = NULL;
    free(s_vis_shd_tris);
    s_vis_shd_tris = NULL;
    free(s_zbuf);
    s_zbuf = NULL;
    free(s_gbuf);
    s_gbuf = NULL;
    free(s_ped_zbuf);
    s_ped_zbuf = NULL;
    free(s_ped_gbuf);
    s_ped_gbuf = NULL;
    free(s_mirr_zbuf);
    s_mirr_zbuf = NULL;
    free(s_mirr_gbuf);
    s_mirr_gbuf = NULL;
    free(s_shd_mask);
    s_shd_mask = NULL;
    free(s_shd_tmp);
    s_shd_tmp = NULL;
    free(s_shd_blur);
    s_shd_blur = NULL;
    s_buf_cap = 0;
    s_mesh_inited = false;

    gl_destroy_context(gl);
    if (wm_is_window_valid(win)) {
        wm_destroy_window(win);
    }
    return 0;
}
