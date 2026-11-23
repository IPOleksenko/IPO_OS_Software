#include <ipo/gfx3d.h>
#include <syscall.h>
#include <threads.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef void (*g3d_job_fn_t)(int tid, int nthreads, void *ctx);

typedef struct {
    ipo_thread_t *threads;
    int worker_count;
    int ctx_refcnt;
    volatile uint32_t epoch;
    volatile uint32_t active_done;
    volatile int stop;
    volatile int hot;
    g3d_job_fn_t fn;
    void *ctx;
} g3d_pool_t;

static g3d_pool_t s_g3d_pool = {0};

static void *g3d_worker_entry(void *arg) {
    int tid = (int)(uintptr_t)arg;
    uint32_t last_ep = 0u;
    for (;;) {
        if (__atomic_load_n(&s_g3d_pool.stop, __ATOMIC_ACQUIRE)) break;
        uint32_t ep = __atomic_load_n(&s_g3d_pool.epoch, __ATOMIC_ACQUIRE);
        if (ep == last_ep) {
            uint32_t budget = __atomic_load_n(&s_g3d_pool.hot, __ATOMIC_RELAXED) ? 250000u : 512u;
            uint32_t spin = 0u;
            while (spin < budget) {
                __asm__ volatile("pause" ::: "memory");
                ep = __atomic_load_n(&s_g3d_pool.epoch, __ATOMIC_ACQUIRE);
                if (ep != last_ep || __atomic_load_n(&s_g3d_pool.stop, __ATOMIC_ACQUIRE)) break;
                spin++;
            }
            if (__atomic_load_n(&s_g3d_pool.stop, __ATOMIC_ACQUIRE)) break;
            if (ep == last_ep) {
                ipo_thread_yield();
                continue;
            }
        }
        last_ep = ep;
        g3d_job_fn_t fn = s_g3d_pool.fn;
        void *ctx = s_g3d_pool.ctx;
        int nthreads = s_g3d_pool.worker_count + 1;
        if (fn) fn(tid, nthreads, ctx);
        __atomic_fetch_add(&s_g3d_pool.active_done, 1u, __ATOMIC_ACQ_REL);
    }
    return NULL;
}

static void g3d_pool_acquire(void) {
    s_g3d_pool.ctx_refcnt++;
    if (s_g3d_pool.worker_count > 0) return;
    uint32_t active_cpus = 0u;
    uint32_t sc_args[1] = { (uint32_t)(uintptr_t)&active_cpus };
    uint32_t cpus = (uint32_t)ipo_syscall(IPO_SYSCALL_SMP_GET_COUNT, 1u, sc_args);
    if (active_cpus > 1u && active_cpus < cpus) cpus = active_cpus;
    if (cpus <= 1u) return;
    uint32_t want = cpus - 1u;
    ipo_thread_t *threads = (ipo_thread_t *)calloc((size_t)want, sizeof(ipo_thread_t));
    if (!threads) return;
    s_g3d_pool.threads = threads;
    __atomic_store_n(&s_g3d_pool.stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_g3d_pool.epoch, 0u, __ATOMIC_RELEASE);
    int created = 0;
    for (uint32_t i = 0u; i < want; i++) {
        if (ipo_thread_create(&s_g3d_pool.threads[i], g3d_worker_entry, (void *)(uintptr_t)(i + 1u)) == 0) {
            created++;
        } else {
            break;
        }
    }
    if (created <= 0) {
        free(s_g3d_pool.threads);
        s_g3d_pool.threads = NULL;
        return;
    }
    s_g3d_pool.worker_count = created;
}

static void g3d_pool_release(void) {
    if (s_g3d_pool.ctx_refcnt > 0) s_g3d_pool.ctx_refcnt--;
    if (s_g3d_pool.ctx_refcnt > 0 || s_g3d_pool.worker_count <= 0) return;
    __atomic_store_n(&s_g3d_pool.stop, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s_g3d_pool.epoch, 1u, __ATOMIC_ACQ_REL);
    for (int i = 0; i < s_g3d_pool.worker_count; i++) {
        ipo_thread_join(s_g3d_pool.threads[i], NULL);
    }
    free(s_g3d_pool.threads);
    s_g3d_pool.threads = NULL;
    s_g3d_pool.worker_count = 0;
}

static void g3d_pool_dispatch(g3d_job_fn_t fn, void *ctx) {
    if (s_g3d_pool.worker_count <= 0) {
        fn(0, 1, ctx);
        return;
    }
    int nthreads = s_g3d_pool.worker_count + 1;
    s_g3d_pool.fn = fn;
    s_g3d_pool.ctx = ctx;
    __atomic_store_n(&s_g3d_pool.active_done, 0u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&s_g3d_pool.epoch, 1u, __ATOMIC_ACQ_REL);
    fn(0, nthreads, ctx);
    uint32_t need = (uint32_t)s_g3d_pool.worker_count;
    uint32_t spin = 0u;
    while (__atomic_load_n(&s_g3d_pool.active_done, __ATOMIC_ACQUIRE) < need) {
        if (++spin > 250000u) {
            ipo_thread_yield();
            spin = 0u;
        } else {
            __asm__ volatile("pause" ::: "memory");
        }
    }
}

static inline void g3d_band_range(int tid, int nthreads, int total, int *y0, int *y1) {
    *y0 = (tid * total) / nthreads;
    *y1 = ((tid + 1) * total) / nthreads;
}

static inline bool ipo_size_mul_ok(size_t a, size_t b, size_t *out) {
    if (a != 0u && b > ((size_t)-1) / a) return false;
    if (out) *out = a * b;
    return true;
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline double g3d_min(double a, double b) { return (a < b) ? a : b; }
static inline double g3d_max(double a, double b) { return (a > b) ? a : b; }

ipo_vec3_t ipo_vec3(double x, double y, double z) {
    ipo_vec3_t r = { x, y, z };
    return r;
}

ipo_vec3_t ipo_vec3_add(ipo_vec3_t a, ipo_vec3_t b) {
    return ipo_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

ipo_vec3_t ipo_vec3_sub(ipo_vec3_t a, ipo_vec3_t b) {
    return ipo_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

ipo_vec3_t ipo_vec3_scale(ipo_vec3_t v, double s) {
    return ipo_vec3(v.x * s, v.y * s, v.z * s);
}

double ipo_vec3_dot(ipo_vec3_t a, ipo_vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

ipo_vec3_t ipo_vec3_cross(ipo_vec3_t a, ipo_vec3_t b) {
    return ipo_vec3(a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x);
}

ipo_vec3_t ipo_vec3_norm(ipo_vec3_t v) {
    double len = sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1e-12) return ipo_vec3_scale(v, 1.0 / len);
    return ipo_vec3(0.0, 1.0, 0.0);
}

ipo_mat4_t ipo_mat4_identity(void) {
    ipo_mat4_t r;
    memset(&r, 0, sizeof(r));
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0;
    return r;
}

ipo_mat4_t ipo_mat4_mul(const ipo_mat4_t *a, const ipo_mat4_t *b) {
    ipo_mat4_t r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            double s = 0.0;
            for (int k = 0; k < 4; k++) {
                s += a->m[k * 4 + row] * b->m[col * 4 + k];
            }
            r.m[col * 4 + row] = s;
        }
    }
    return r;
}

ipo_mat4_t ipo_mat4_translate(double x, double y, double z) {
    ipo_mat4_t r = ipo_mat4_identity();
    r.m[12] = x; r.m[13] = y; r.m[14] = z;
    return r;
}

ipo_mat4_t ipo_mat4_scale(double sx, double sy, double sz) {
    ipo_mat4_t r = ipo_mat4_identity();
    r.m[0] = sx; r.m[5] = sy; r.m[10] = sz;
    return r;
}

ipo_mat4_t ipo_mat4_rotate_x(double rad) {
    ipo_mat4_t r = ipo_mat4_identity();
    double c = cos(rad), s = sin(rad);
    r.m[5] = c; r.m[6] = s;
    r.m[9] = -s; r.m[10] = c;
    return r;
}

ipo_mat4_t ipo_mat4_rotate_y(double rad) {
    ipo_mat4_t r = ipo_mat4_identity();
    double c = cos(rad), s = sin(rad);
    r.m[0] = c; r.m[2] = -s;
    r.m[8] = s; r.m[10] = c;
    return r;
}

ipo_mat4_t ipo_mat4_rotate_z(double rad) {
    ipo_mat4_t r = ipo_mat4_identity();
    double c = cos(rad), s = sin(rad);
    r.m[0] = c; r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

ipo_mat4_t ipo_mat4_perspective(double fovy_rad, double aspect, double znear, double zfar) {
    ipo_mat4_t r;
    memset(&r, 0, sizeof(r));
    double f = 1.0 / tan(fovy_rad * 0.5);
    r.m[0] = f / (aspect > 1e-6 ? aspect : 1.0);
    r.m[5] = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0;
    r.m[14] = (2.0 * zfar * znear) / (znear - zfar);
    return r;
}

ipo_mat4_t ipo_mat4_ortho(double left, double right, double bottom, double top, double znear, double zfar) {
    ipo_mat4_t r = ipo_mat4_identity();
    r.m[0] = 2.0 / (right - left);
    r.m[5] = 2.0 / (top - bottom);
    r.m[10] = -2.0 / (zfar - znear);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -(zfar + znear) / (zfar - znear);
    return r;
}

ipo_mat4_t ipo_mat4_look_at(ipo_vec3_t eye, ipo_vec3_t target, ipo_vec3_t up) {
    ipo_vec3_t f = ipo_vec3_norm(ipo_vec3_sub(target, eye));
    ipo_vec3_t s = ipo_vec3_norm(ipo_vec3_cross(f, up));
    ipo_vec3_t u = ipo_vec3_cross(s, f);
    ipo_mat4_t r = ipo_mat4_identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8]  = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9]  = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -ipo_vec3_dot(s, eye);
    r.m[13] = -ipo_vec3_dot(u, eye);
    r.m[14] =  ipo_vec3_dot(f, eye);
    return r;
}

static inline ipo_vec4_t mat4_mul_vec4(const ipo_mat4_t *m, ipo_vec4_t v) {
    ipo_vec4_t r;
    r.x = m->m[0]*v.x + m->m[4]*v.y + m->m[8]*v.z  + m->m[12]*v.w;
    r.y = m->m[1]*v.x + m->m[5]*v.y + m->m[9]*v.z  + m->m[13]*v.w;
    r.z = m->m[2]*v.x + m->m[6]*v.y + m->m[10]*v.z + m->m[14]*v.w;
    r.w = m->m[3]*v.x + m->m[7]*v.y + m->m[11]*v.z + m->m[15]*v.w;
    return r;
}

static inline ipo_vec3_t mat4_mul_dir3(const ipo_mat4_t *m, ipo_vec3_t v) {
    return ipo_vec3(m->m[0]*v.x + m->m[4]*v.y + m->m[8]*v.z,
                    m->m[1]*v.x + m->m[5]*v.y + m->m[9]*v.z,
                    m->m[2]*v.x + m->m[6]*v.y + m->m[10]*v.z);
}

bool ipo_gfx3d_ensure_lights(ipo_gfx3d_ctx_t *ctx, uint32_t min_lights) {
    if (!ctx) return false;
    if (min_lights <= ctx->lights_cap && ctx->lights) return true;
    uint32_t new_cap = ctx->lights_cap ? ctx->lights_cap : 8u;
    while (new_cap < min_lights) {
        if (new_cap > UINT32_MAX / 2u) {
            new_cap = min_lights;
            break;
        }
        new_cap *= 2u;
    }
    size_t alloc_bytes = 0u;
    if (!ipo_size_mul_ok((size_t)new_cap, sizeof(ipo_gfx3d_light_t), &alloc_bytes)) {
        return false;
    }
    ipo_gfx3d_light_t *nl = (ipo_gfx3d_light_t *)realloc(ctx->lights, alloc_bytes);
    if (!nl) return false;
    memset(nl + ctx->lights_cap, 0, (size_t)(new_cap - ctx->lights_cap) * sizeof(ipo_gfx3d_light_t));
    ctx->lights = nl;
    ctx->lights_cap = new_cap;
    return true;
}

bool ipo_gfx3d_set_light(ipo_gfx3d_ctx_t *ctx, uint32_t index, const ipo_gfx3d_light_t *light) {
    if (!ctx || !light || index == UINT32_MAX) return false;
    if (!ipo_gfx3d_ensure_lights(ctx, index + 1u)) return false;
    ctx->lights[index] = *light;
    return true;
}

ipo_gfx3d_ctx_t *ipo_gfx3d_create(int width, int height) {
    if (width <= 0) width = 320;
    if (height <= 0) height = 200;
    ipo_gfx3d_ctx_t *ctx = (ipo_gfx3d_ctx_t *)calloc(1u, sizeof(ipo_gfx3d_ctx_t));
    if (!ctx) return NULL;
    ctx->struct_size = (uint32_t)sizeof(ipo_gfx3d_ctx_t);
    ctx->view = ipo_mat4_identity();
    ctx->proj = ipo_mat4_perspective(50.0 * M_PI / 180.0, (double)width / (double)height, 0.1, 100.0);
    ctx->cull_mode = IPO_GFX3D_CULL_BACK;
    ctx->depth_test = true;
    ctx->depth_write = true;
    ctx->enable_env_reflection = true;
    ctx->ambient_sky = (ipo_rgba64f_t){ 0.22, 0.28, 0.38, 1.0 };
    ctx->ambient_ground = (ipo_rgba64f_t){ 0.08, 0.07, 0.06, 1.0 };
    if (!ipo_gfx3d_ensure_lights(ctx, 8u)) {
        free(ctx);
        return NULL;
    }
    ctx->lights[0].enabled = true;
    ctx->lights[0].is_point = false;
    ctx->lights[0].pos_or_dir = ipo_vec3_norm(ipo_vec3(0.6, 1.0, 0.7));
    ctx->lights[0].color = (ipo_rgba64f_t){ 3.2, 3.05, 2.85, 1.0 };
    if (!ipo_gfx3d_resize(ctx, width, height)) {
        free(ctx->lights);
        free(ctx);
        return NULL;
    }
    g3d_pool_acquire();
    return ctx;
}

typedef struct {
    const ipo_gfx3d_ctx_t *ctx;
    bool                   postfx_done;
} g3d_ctx_fx_state_t;

static g3d_ctx_fx_state_t *s_fx_states = NULL;
static size_t              s_fx_count  = 0u;
static size_t              s_fx_cap    = 0u;

static void g3d_set_postfx_done(const ipo_gfx3d_ctx_t *ctx, bool done) {
    if (!ctx) return;
    for (size_t i = 0u; i < s_fx_count; i++) {
        if (s_fx_states[i].ctx == ctx) {
            s_fx_states[i].postfx_done = done;
            return;
        }
    }
    if (s_fx_count >= s_fx_cap) {
        size_t ncap = s_fx_cap ? (s_fx_cap * 2u) : 8u;
        size_t bytes = 0u;
        if (ncap <= s_fx_cap || !ipo_size_mul_ok(ncap, sizeof(g3d_ctx_fx_state_t), &bytes)) return;
        g3d_ctx_fx_state_t *nb = (g3d_ctx_fx_state_t *)realloc(s_fx_states, bytes);
        if (!nb) return;
        s_fx_states = nb;
        s_fx_cap = ncap;
    }
    s_fx_states[s_fx_count].ctx = ctx;
    s_fx_states[s_fx_count].postfx_done = done;
    s_fx_count++;
}

static bool g3d_is_postfx_done(const ipo_gfx3d_ctx_t *ctx) {
    if (!ctx) return true;
    for (size_t i = 0u; i < s_fx_count; i++) {
        if (s_fx_states[i].ctx == ctx) return s_fx_states[i].postfx_done;
    }
    return false;
}

static void g3d_drop_postfx_state(const ipo_gfx3d_ctx_t *ctx) {
    for (size_t i = 0u; i < s_fx_count; i++) {
        if (s_fx_states[i].ctx == ctx) {
            s_fx_states[i] = s_fx_states[s_fx_count - 1u];
            s_fx_count--;
            return;
        }
    }
}

void ipo_gfx3d_destroy(ipo_gfx3d_ctx_t *ctx) {
    if (!ctx) return;
    g3d_pool_release();
    g3d_drop_postfx_state(ctx);
    extern void g3d_gpu_destroy_ctx(ipo_gfx3d_ctx_t *ctx);
    g3d_gpu_destroy_ctx(ctx);
    free(ctx->lights);
    free(ctx->color_fb);
    free(ctx->z_buffer);
    free(ctx);
}

bool ipo_gfx3d_resize(ipo_gfx3d_ctx_t *ctx, int width, int height) {
    if (!ctx || width <= 0 || height <= 0) return false;
    size_t n = (size_t)width * (size_t)height;
    ipo_rgba64f_t *cfb = (ipo_rgba64f_t *)realloc(ctx->color_fb, n * sizeof(ipo_rgba64f_t));
    float *zb = (float *)realloc(ctx->z_buffer, n * sizeof(float));
    if (!cfb || !zb) {
        if (cfb) ctx->color_fb = cfb;
        if (zb) ctx->z_buffer = zb;
        return false;
    }
    ctx->color_fb = cfb;
    ctx->z_buffer = zb;
    ctx->width = width;
    ctx->height = height;
    g3d_set_postfx_done(ctx, false);
    extern void g3d_gpu_invalidate_rt(ipo_gfx3d_ctx_t *ctx);
    g3d_gpu_invalidate_rt(ctx);
    return true;
}

void ipo_gfx3d_clear(ipo_gfx3d_ctx_t *ctx, ipo_rgba64f_t sky_top, ipo_rgba64f_t sky_horizon, double depth) {
    g3d_set_postfx_done(ctx, false);
    extern bool g3d_gpu_clear(ipo_gfx3d_ctx_t *ctx, ipo_rgba64f_t sky_top, ipo_rgba64f_t sky_horizon, double depth);
    if (g3d_gpu_clear(ctx, sky_top, sky_horizon, depth)) return;
    if (!ctx || !ctx->color_fb || !ctx->z_buffer) return;
    __atomic_store_n(&s_g3d_pool.hot, 1, __ATOMIC_RELEASE);
    float dval = (float)depth;
    int w = ctx->width, h = ctx->height;
    double inv_hm1 = (h > 1) ? (1.0 / (double)(h - 1)) : 1.0;
    for (int y = 0; y < h; y++) {
        double t = (double)y * inv_hm1;
        double blend = sqrt(t * sqrt(t));
        ipo_rgba64f_t row_c = {
            sky_top.r * (1.0 - blend) + sky_horizon.r * blend,
            sky_top.g * (1.0 - blend) + sky_horizon.g * blend,
            sky_top.b * (1.0 - blend) + sky_horizon.b * blend,
            1.0
        };
        ipo_rgba64f_t *cr = ctx->color_fb + (size_t)y * (size_t)w;
        float *zr = ctx->z_buffer + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            cr[x] = row_c;
            zr[x] = dval;
        }
    }
    ctx->tris_submitted = 0;
    ctx->tris_drawn = 0;
}

void ipo_gfx3d_clear_depth(ipo_gfx3d_ctx_t *ctx, double depth) {
    if (!ctx) return;
    extern bool g3d_gpu_clear_depth(ipo_gfx3d_ctx_t *ctx, double depth);
    if (g3d_gpu_clear_depth(ctx, depth)) return;
    if (!ctx->z_buffer || ctx->width <= 0 || ctx->height <= 0) return;
    float dval = (float)depth;
    size_t total = (size_t)ctx->width * (size_t)ctx->height;
    for (size_t i = 0u; i < total; i++) {
        ctx->z_buffer[i] = dval;
    }
}

static uint16_t s_g3d_rsqrt_q20[4096];
static uint16_t s_g3d_fresnel_q12[4097];
static float    s_g3d_aces_gamma_lut[1025];
static float    s_g3d_inv_aces_lut[513];
static bool     s_g3d_luts_ready = false;

static void g3d_init_luts(void) {
    if (s_g3d_luts_ready) return;
    s_g3d_rsqrt_q20[0] = 65535u;
    for (int i = 1; i < 4096; i++) {
        uint32_t v = (uint32_t)(1048576.0 / sqrt((double)i) + 0.5);
        if (v > 65535u) v = 65535u;
        s_g3d_rsqrt_q20[i] = (uint16_t)v;
    }
    for (int i = 0; i <= 4096; i++) {
        double om = 1.0 - (double)i * (1.0 / 4096.0);
        double om2 = om * om;
        s_g3d_fresnel_q12[i] = (uint16_t)(om2 * om2 * om * 4096.0 + 0.5);
    }
    for (int i = 0; i <= 1024; i++) {
        double x = (double)i * (8.0 / 1024.0);
        double t = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
        if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
        s_g3d_aces_gamma_lut[i] = (float)sqrt(t);
    }
    for (int i = 0; i <= 512; i++) {
        double g = (double)i / 512.0;
        if (g >= 0.992) g = 0.992;
        double t = g * g;
        if (t >= 0.985) t = 0.985;
        double a = 2.51 - 2.43 * t;
        double b = 0.03 - 0.59 * t;
        double c = -0.14 * t;
        double disc = b * b - 4.0 * a * c;
        if (disc < 0.0) disc = 0.0;
        double x = (-b + sqrt(disc)) / (2.0 * a);
        s_g3d_inv_aces_lut[i] = (x > 0.0) ? (float)x : 0.0f;
    }
    s_g3d_luts_ready = true;
}

/* Normalize integer vector into Q14 [-16384..16384] using 8KB CLZ rsqrt LUT (avoids TCG softfloat). */
static inline void g3d_norm_q14(int32_t nx, int32_t ny, int32_t nz,
                                int32_t *ox, int32_t *oy, int32_t *oz) {
    int32_t ax = nx < 0 ? -nx : nx;
    int32_t ay = ny < 0 ? -ny : ny;
    int32_t az = nz < 0 ? -nz : nz;
    int32_t m = ax > ay ? (ax > az ? ax : az) : (ay > az ? ay : az);
    while (m > 32767) {
        nx >>= 1; ny >>= 1; nz >>= 1; m >>= 1;
    }
    uint32_t len2 = (uint32_t)(nx * nx) + (uint32_t)(ny * ny) + (uint32_t)(nz * nz);
    if (len2 == 0u) {
        *ox = 0; *oy = 0; *oz = 0;
        return;
    }
    int e = 31 - __builtin_clz(len2);
    uint32_t inv;
    int shift;
    if (e >= 10) {
        int p = (e - 10) & ~1;
        uint32_t idx = len2 >> p;
        if (idx >= 4096u) idx = 4095u;
        inv = (uint32_t)s_g3d_rsqrt_q20[idx];
        shift = 6 + (p >> 1);
    } else {
        int p = (10 - e + 1) & ~1;
        uint32_t idx = len2 << p;
        if (idx >= 4096u) idx = 4095u;
        inv = (uint32_t)s_g3d_rsqrt_q20[idx];
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

/* Convert non-negative Q12 integer to IEEE-754 binary64 without SSE float helpers. */
static inline void g3d_write_q12_f64(double *dst, int32_t v) {
    if (v <= 0) {
        uint32_t z[2] = { 0u, 0u };
        memcpy(dst, z, 8);
        return;
    }
    if (v >= 1048576) {
        *dst = (double)v * (1.0 / 4096.0);
        return;
    }
    uint32_t uv = (uint32_t)v;
    int e = 31 - __builtin_clz(uv);
    uint32_t hi = ((uint32_t)(1011 + e) << 20) | ((uv << (20 - e)) & 0xFFFFFu);
    uint32_t pair[2] = { 0u, hi };
    memcpy(dst, pair, 8);
}

static inline uint32_t g3d_z_q24_to_f32bits(int32_t z_q24) {
    if (z_q24 <= 0) return 0u;
    uint32_t uz = (uint32_t)z_q24;
    int ze = 31 - __builtin_clz(uz);
    uint32_t zmant = (ze >= 23) ? (uz >> (ze - 23)) : (uz << (23 - ze));
    return ((uint32_t)(127 + ze - 24) << 23) | (zmant & 0x7FFFFFu);
}

typedef struct {
    const ipo_gfx3d_texture_t *tex_ptr;
    const ipo_rgba64f_t       *texels_ptr;
    int                        width, height;
    double                     sig0, sig1;
    int16_t                   *q12;
    size_t                     cap;
} g3d_tex_cache_t;

static g3d_tex_cache_t *s_g3d_tex_cache = NULL;
static size_t           s_g3d_tex_count = 0u;
static size_t           s_g3d_tex_cap   = 0u;

static const int16_t *g3d_get_tex_q12(const ipo_gfx3d_texture_t *tex) {
    if (!tex || !tex->texels || tex->width <= 0 || tex->height <= 0) return NULL;
    size_t n = 0u, need = 0u, alloc_bytes = 0u;
    if (!ipo_size_mul_ok((size_t)tex->width, (size_t)tex->height, &n) || n == 0u) return NULL;
    if (!ipo_size_mul_ok(n, 4u, &need) || !ipo_size_mul_ok(need, sizeof(int16_t), &alloc_bytes)) return NULL;
    double s0 = tex->texels[0].r;
    double s1 = tex->texels[n - 1u].b;
    g3d_tex_cache_t *dst = NULL;
    for (size_t i = 0; i < s_g3d_tex_count; i++) {
        g3d_tex_cache_t *c = &s_g3d_tex_cache[i];
        if (c->tex_ptr == tex) {
            if (c->texels_ptr == tex->texels &&
                c->width == tex->width && c->height == tex->height &&
                c->sig0 == s0 && c->sig1 == s1 && c->q12) {
                return c->q12;
            }
            dst = c;
            break;
        }
    }
    if (!dst) {
        if (s_g3d_tex_count >= s_g3d_tex_cap) {
            size_t new_cap = s_g3d_tex_cap ? (s_g3d_tex_cap * 2u) : 4u;
            size_t arr_bytes = 0u;
            if (new_cap <= s_g3d_tex_cap || !ipo_size_mul_ok(new_cap, sizeof(g3d_tex_cache_t), &arr_bytes)) {
                return NULL;
            }
            g3d_tex_cache_t *nc = (g3d_tex_cache_t *)realloc(s_g3d_tex_cache, arr_bytes);
            if (!nc) return NULL;
            memset(nc + s_g3d_tex_cap, 0, (new_cap - s_g3d_tex_cap) * sizeof(g3d_tex_cache_t));
            s_g3d_tex_cache = nc;
            s_g3d_tex_cap = new_cap;
        }
        dst = &s_g3d_tex_cache[s_g3d_tex_count++];
    }
    if (need > dst->cap) {
        int16_t *nb = (int16_t *)realloc(dst->q12, alloc_bytes);
        if (!nb) return NULL;
        dst->q12 = nb;
        dst->cap = need;
    }
    for (size_t i = 0; i < n; i++) {
        double r = tex->texels[i].r * 4096.0 + 0.5;
        double g = tex->texels[i].g * 4096.0 + 0.5;
        double b = tex->texels[i].b * 4096.0 + 0.5;
        int ir = (r < 0.0) ? 0 : (r > 32767.0 ? 32767 : (int)r);
        int ig = (g < 0.0) ? 0 : (g > 32767.0 ? 32767 : (int)g);
        int ib = (b < 0.0) ? 0 : (b > 32767.0 ? 32767 : (int)b);
        dst->q12[i * 4u + 0u] = (int16_t)ir;
        dst->q12[i * 4u + 1u] = (int16_t)ig;
        dst->q12[i * 4u + 2u] = (int16_t)ib;
        dst->q12[i * 4u + 3u] = 4096;
    }
    dst->tex_ptr = tex;
    dst->texels_ptr = tex->texels;
    dst->width = tex->width;
    dst->height = tex->height;
    dst->sig0 = s0;
    dst->sig1 = s1;
    return dst->q12;
}

typedef struct {
    float    rough;
    uint32_t lut[1026]; /* Q12 GGX NDF across NdotH [0..1] (step 1/1024), clamped to 256.0 */
} g3d_ggx_slot_t;

static g3d_ggx_slot_t *s_g3d_ggx_slots = NULL;
static size_t          s_g3d_ggx_used  = 0u;
static size_t          s_g3d_ggx_cap   = 0u;

static const uint32_t *g3d_get_ggx_lut(float rough) {
    for (size_t i = 0; i < s_g3d_ggx_used; i++) {
        float d = s_g3d_ggx_slots[i].rough - rough;
        if (d > -1e-5f && d < 1e-5f) return s_g3d_ggx_slots[i].lut;
    }
    if (s_g3d_ggx_used >= s_g3d_ggx_cap) {
        size_t new_cap = s_g3d_ggx_cap ? (s_g3d_ggx_cap * 2u) : 8u;
        size_t alloc_bytes = 0u;
        if (new_cap <= s_g3d_ggx_cap || !ipo_size_mul_ok(new_cap, sizeof(g3d_ggx_slot_t), &alloc_bytes)) {
            return s_g3d_ggx_used ? s_g3d_ggx_slots[0].lut : NULL;
        }
        g3d_ggx_slot_t *ns = (g3d_ggx_slot_t *)realloc(s_g3d_ggx_slots, alloc_bytes);
        if (!ns) return s_g3d_ggx_used ? s_g3d_ggx_slots[0].lut : NULL;
        s_g3d_ggx_slots = ns;
        s_g3d_ggx_cap = new_cap;
    }
    g3d_ggx_slot_t *s = &s_g3d_ggx_slots[s_g3d_ggx_used++];
    s->rough = rough;
    double a = (double)rough * (double)rough;
    double a2 = a * a;
    double a2_m1 = a2 - 1.0;
    double a2_inv_pi = a2 * 0.3183098861837907;
    for (int i = 0; i <= 1024; i++) {
        double nh = (double)i * (1.0 / 1024.0);
        double den = (nh * nh) * a2_m1 + 1.0;
        double spec = a2_inv_pi / (den * den + 1e-12);
        double q = spec * 4096.0 + 0.5;
        if (q > 1048575.0) q = 1048575.0;
        s->lut[i] = (uint32_t)q;
    }
    s->lut[1025] = s->lut[1024];
    return s->lut;
}

static inline void sample_tex_bilinear_q12(const int16_t *tex_q12, int tw, int th,
                                           int32_t u_q16, int32_t v_q16,
                                           int32_t *out_r, int32_t *out_g, int32_t *out_b) {
    uint32_t uf = (uint32_t)u_q16 & 0xFFFFu;
    uint32_t vf = (uint32_t)v_q16 & 0xFFFFu;
    uint32_t fx_all = uf * (uint32_t)(tw - 1);
    uint32_t fy_all = vf * (uint32_t)(th - 1);
    int x0 = (int)(fx_all >> 16);
    int y0 = (int)(fy_all >> 16);
    int x1 = (x0 + 1 < tw) ? x0 + 1 : 0;
    int y1 = (y0 + 1 < th) ? y0 + 1 : 0;
    int32_t tx = (int32_t)((fx_all >> 8) & 255u);
    int32_t ty = (int32_t)((fy_all >> 8) & 255u);
    const int16_t *t00 = &tex_q12[((size_t)y0 * (size_t)tw + (size_t)x0) * 4u];
    const int16_t *t10 = &tex_q12[((size_t)y0 * (size_t)tw + (size_t)x1) * 4u];
    const int16_t *t01 = &tex_q12[((size_t)y1 * (size_t)tw + (size_t)x0) * 4u];
    const int16_t *t11 = &tex_q12[((size_t)y1 * (size_t)tw + (size_t)x1) * 4u];
    for (int c = 0; c < 3; c++) {
        int32_t top = ((int32_t)t00[c] << 8) + tx * ((int32_t)t10[c] - (int32_t)t00[c]);
        int32_t bot = ((int32_t)t01[c] << 8) + tx * ((int32_t)t11[c] - (int32_t)t01[c]);
        int32_t val = ((top << 8) + ty * (bot - top)) >> 16;
        if (c == 0) *out_r = val;
        else if (c == 1) *out_g = val;
        else *out_b = val;
    }
}

static inline float g3d_rsqrtf(float x) {
    if (!(x > 1e-20f)) return 0.0f;
#ifdef __SSE2__
    float y;
    __asm__ ("rsqrtss %1, %0" : "=x"(y) : "x"(x));
    return y * (1.5f - 0.5f * x * y * y);
#else
    return 1.0f / sqrtf(x);
#endif
}

static inline int g3d_clamp_floor(float v, int lo, int hi) {
    if (v <= (float)lo) return lo;
    if (v >= (float)hi) return hi;
    int i = (int)v;
    return (v < (float)i) ? (i - 1) : i;
}

static inline int g3d_clamp_ceil(float v, int lo, int hi) {
    if (v <= (float)lo) return lo;
    if (v >= (float)hi) return hi;
    int i = (int)v;
    return (v > (float)i) ? (i + 1) : i;
}

static inline float g3d_smoothstepf(float e0, float e1, float x) {
    float t = (x - e0) / (e1 - e0);
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static inline void sample_env_sky_f(float dir_x, float dir_y, float dir_z,
                                    const ipo_gfx3d_ctx_t *ctx,
                                    bool has_l0, float l0x, float l0y, float l0z,
                                    float *out_r, float *out_g, float *out_b) {
    float ty = 0.5f * (dir_y + 1.0f);
    if (ty < 0.0f) ty = 0.0f; else if (ty > 1.0f) ty = 1.0f;
    float omt = 1.0f - ty;
    float r = (float)ctx->ambient_ground.r * omt + (float)ctx->ambient_sky.r * ty;
    float g = (float)ctx->ambient_ground.g * omt + (float)ctx->ambient_sky.g * ty;
    float b = (float)ctx->ambient_ground.b * omt + (float)ctx->ambient_sky.b * ty;

    /* Subtle dark studio horizon wrap */
    float hor = expf(-fabsf(dir_y) * 5.5f);
    r += 0.14f * hor;
    g += 0.17f * hor;
    b += 0.24f * hor;

    /* Overhead Silk Canopy Softbox with crisp architectural diffuser muntins */
    float ov_y = g3d_smoothstepf(0.52f, 0.72f, dir_y);
    float ov_x = 1.0f - g3d_smoothstepf(0.52f, 0.78f, fabsf(dir_x));
    float ov_z = 1.0f - g3d_smoothstepf(0.52f, 0.78f, fabsf(dir_z));
    float canopy = ov_y * ov_x * ov_z;
    if (canopy > 0.001f) {
        float bar_x = g3d_smoothstepf(0.015f, 0.048f, fabsf(dir_x));
        float bar_z = g3d_smoothstepf(0.015f, 0.048f, fabsf(dir_z));
        float muntin = 0.12f + 0.88f * bar_x * bar_z;
        float glow = canopy * muntin * 3.85f;
        r += glow * 1.02f;
        g += glow * 0.99f;
        b += glow * 0.94f;
    }

    /* Warm Key Softbox Right Strip */
    float rk_x = g3d_smoothstepf(0.50f, 0.72f, dir_x);
    float rk_y = g3d_smoothstepf(-0.20f, 0.12f, dir_y) * (1.0f - g3d_smoothstepf(0.45f, 0.75f, dir_y));
    float rk_z = 1.0f - g3d_smoothstepf(0.42f, 0.72f, fabsf(dir_z - 0.18f));
    float rkey = rk_x * rk_y * rk_z;
    if (rkey > 0.001f) {
        r += rkey * 2.55f;
        g += rkey * 2.38f;
        b += rkey * 2.10f;
    }

    /* Cool Rim Softbox Left Strip */
    float lc_x = g3d_smoothstepf(0.54f, 0.76f, -dir_x);
    float lc_y = g3d_smoothstepf(-0.26f, 0.08f, dir_y) * (1.0f - g3d_smoothstepf(0.42f, 0.72f, dir_y));
    float lc_z = 1.0f - g3d_smoothstepf(0.42f, 0.74f, fabsf(dir_z + 0.12f));
    float lrim = lc_x * lc_y * lc_z;
    if (lrim > 0.001f) {
        r += lrim * 0.95f;
        g += lrim * 1.48f;
        b += lrim * 2.45f;
    }

    if (has_l0) {
        float d = dir_x * l0x + dir_y * l0y + dir_z * l0z;
        if (d > 0.75f) {
            float p = (d - 0.75f) * 4.0f;
            float lobe = p * p * 1.8f;
            r += (float)ctx->lights[0].color.r * lobe;
            g += (float)ctx->lights[0].color.g * lobe;
            b += (float)ctx->lights[0].color.b * lobe;
        }
    }
    *out_r = r;
    *out_g = g;
    *out_b = b;
}

static inline void sample_tex_bilinear_f(const ipo_gfx3d_texture_t *tex, float u, float v,
                                         float *out_r, float *out_g, float *out_b) {
    if (!tex || !tex->texels || tex->width <= 0 || tex->height <= 0) {
        *out_r = 1.0f; *out_g = 1.0f; *out_b = 1.0f;
        return;
    }
    int iu = (int)u; if (u < (float)iu) iu--; u -= (float)iu;
    int iv = (int)v; if (v < (float)iv) iv--; v -= (float)iv;
    float fx = u * (float)(tex->width - 1);
    float fy = v * (float)(tex->height - 1);
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = (x0 + 1 < tex->width) ? x0 + 1 : 0;
    int y1 = (y0 + 1 < tex->height) ? y0 + 1 : 0;
    float tx = fx - (float)x0, ty = fy - (float)y0;
    float w00 = (1.0f - tx) * (1.0f - ty), w10 = tx * (1.0f - ty);
    float w01 = (1.0f - tx) * ty,         w11 = tx * ty;
    const ipo_rgba64f_t *t00 = &tex->texels[(size_t)y0 * (size_t)tex->width + x0];
    const ipo_rgba64f_t *t10 = &tex->texels[(size_t)y0 * (size_t)tex->width + x1];
    const ipo_rgba64f_t *t01 = &tex->texels[(size_t)y1 * (size_t)tex->width + x0];
    const ipo_rgba64f_t *t11 = &tex->texels[(size_t)y1 * (size_t)tex->width + x1];
    *out_r = (float)t00->r * w00 + (float)t10->r * w10 + (float)t01->r * w01 + (float)t11->r * w11;
    *out_g = (float)t00->g * w00 + (float)t10->g * w10 + (float)t01->g * w01 + (float)t11->g * w11;
    *out_b = (float)t00->b * w00 + (float)t10->b * w10 + (float)t01->b * w01 + (float)t11->b * w11;
}

typedef struct {
    float sx, sy, sz, iw;
    float wx, wy, wz;
    float nx, ny, nz;
    float cr, cg, cb;
    float u, v;
    uint8_t valid;
} g3d_xform_v_t;

typedef struct {
    int32_t cr, cg, cb;
    int32_t x, y, z;
    uint8_t is_point;
} g3d_light_pre_t;

typedef struct {
    int min_x, max_x, min_y, max_y;
    uint8_t use_q12;
    union {
        struct {
            int32_t iw0_q14, iw1_q14, iw2_q14;
            int32_t w0_row_q, w1_row_q, dw0_dx_q, dw1_dx_q, dw0_dy_q, dw1_dy_q;
            int32_t sz0_q24, sz1_q24, sz2_q24;
            int32_t wx0_q, wy0_q, wz0_q, wx1_q, wy1_q, wz1_q, wx2_q, wy2_q, wz2_q;
            int32_t nx0_q, ny0_q, nz0_q, nx1_q, ny1_q, nz1_q, nx2_q, ny2_q, nz2_q;
            int32_t cr0_q, cg0_q, cb0_q, cr1_q, cg1_q, cb1_q, cr2_q, cg2_q, cb2_q;
            int32_t u0_q, v0_q, u1_q, v1_q, u2_q, v2_q;
        } q;
        struct {
            float iw0, iw1, iw2, sz0, sz1, sz2;
            float w0_row, w1_row, dw0_dx, dw1_dx, dw0_dy, dw1_dy;
            float wx0, wy0, wz0, wx1, wy1, wz1, wx2, wy2, wz2;
            float nx0, ny0, nz0, nx1, ny1, nz1, nx2, ny2, nz2;
            float cr0, cg0, cb0, cr1, cg1, cb1, cr2, cg2, cb2;
            float u0, v0t, u1, v1t, u2, v2t;
        } f;
    };
} g3d_mesh_tri_t;

typedef struct {
    const ipo_gfx3d_mesh_t *mesh;
    const ipo_mat4_t       *mvp;
    const ipo_mat4_t       *model;
    const ipo_mat4_t       *norm_mat;
    g3d_xform_v_t          *xf;
    float                   wf, hf;
} g3d_xform_job_t;

static void g3d_xform_verts_worker(int tid, int nthreads, void *opaque) {
    const g3d_xform_job_t *j = (const g3d_xform_job_t *)opaque;
    int v0 = 0, v1 = 0;
    g3d_band_range(tid, nthreads, (int)j->mesh->vert_count, &v0, &v1);
    float wf = j->wf, hf = j->hf;
    for (int vi = v0; vi < v1; vi++) {
        const ipo_gfx3d_vertex_t *v = &j->mesh->verts[vi];
        ipo_vec4_t c = mat4_mul_vec4(j->mvp, (ipo_vec4_t){v->pos.x, v->pos.y, v->pos.z, 1.0});
        g3d_xform_v_t *o = &j->xf[vi];
        if (c.w <= 0.05) {
            o->valid = 0u;
            continue;
        }
        float iw = (float)(1.0 / c.w);
        o->valid = 1u;
        o->iw = iw;
        o->sx = ((float)c.x * iw * 0.5f + 0.5f) * wf;
        o->sy = (0.5f - (float)c.y * iw * 0.5f) * hf;
        o->sz = ((float)c.z * iw) * 0.5f + 0.5f;
        ipo_vec4_t wp = mat4_mul_vec4(j->model, (ipo_vec4_t){v->pos.x, v->pos.y, v->pos.z, 1.0});
        ipo_vec3_t wn = ipo_vec3_norm(mat4_mul_dir3(j->norm_mat, v->normal));
        o->wx = (float)wp.x; o->wy = (float)wp.y; o->wz = (float)wp.z;
        o->nx = (float)wn.x; o->ny = (float)wn.y; o->nz = (float)wn.z;
        o->cr = (float)v->color.r; o->cg = (float)v->color.g; o->cb = (float)v->color.b;
        o->u = (float)v->u; o->v = (float)v->v;
    }
}

typedef struct {
    ipo_gfx3d_ctx_t           *ctx;
    int                        w, h;
    const g3d_mesh_tri_t      *tris;
    uint32_t                   tri_count;
    float                      a2_m1, a2_inv_pi, metal, ao, kd, refl_w, amb_w;
    bool                       has_refl, is_translucent, env_has_l0;
    float                      amb_gr, amb_gg, amb_gb, amb_sr, amb_sg, amb_sb;
    float                      em_r, em_g, em_b, mat_ar, mat_ag, mat_ab;
    double                     mat_aa;
    float                      cam_x, cam_y, cam_z;
    float                      env_l0x, env_l0y, env_l0z;
    const ipo_gfx3d_texture_t *albedo_tex;
    const int16_t             *tex_q12;
    int                        tex_w, tex_h;
    const uint32_t            *ggx_lut;
    const ipo_vec3_t          *pre_L;
    const uint32_t            *active_lights;
    const g3d_light_pre_t     *pre_lq;
    int                        num_active_lights;
    int32_t                    metal_q12, kd_q12, f0_base_q12, amb_w_q12, refl_ao_q12;
    int32_t                    em_r_q12, em_g_q12, em_b_q12;
    int32_t                    mat_ar_q12, mat_ag_q12, mat_ab_q12;
    int32_t                    amb_gr_q12, amb_gg_q12, amb_gb_q12;
    int32_t                    amb_sr_q12, amb_sg_q12, amb_sb_q12;
    int32_t                    env_l0x_q14, env_l0y_q14, env_l0z_q14;
    int32_t                    l0_cr_q12, l0_cg_q12, l0_cb_q12;
    int32_t                    cam_x_q8, cam_y_q8, cam_z_q8;
} g3d_mesh_job_t;

static void g3d_mesh_band_worker(int tid, int nthreads, void *opaque) {
    const g3d_mesh_job_t *j = (const g3d_mesh_job_t *)opaque;
    int band_y0 = 0, band_y1 = 0;
    g3d_band_range(tid, nthreads, j->h, &band_y0, &band_y1);
    if (band_y0 >= band_y1) return;

    ipo_gfx3d_ctx_t *ctx = j->ctx;
    int w = j->w;
    bool depth_test = ctx->depth_test;
    bool depth_write = ctx->depth_write;
    bool is_translucent = j->is_translucent;
    bool has_refl = j->has_refl;
    bool env_has_l0 = j->env_has_l0;
    int num_active_lights = j->num_active_lights;
    const g3d_light_pre_t *pre_lq = j->pre_lq;
    const ipo_vec3_t *pre_L = j->pre_L;
    const uint32_t *active_lights = j->active_lights;
    const int16_t *tex_q12 = j->tex_q12;
    int tex_w = j->tex_w, tex_h = j->tex_h;
    const uint32_t *ggx_lut = j->ggx_lut;
    const ipo_gfx3d_texture_t *albedo_tex = j->albedo_tex;
    double mat_aa = j->mat_aa;

    const int32_t metal_q12   = j->metal_q12;
    const int32_t kd_q12      = j->kd_q12;
    const int32_t f0_base_q12 = j->f0_base_q12;
    const int32_t amb_w_q12   = j->amb_w_q12;
    const int32_t refl_ao_q12 = j->refl_ao_q12;
    const int32_t em_r_q12    = j->em_r_q12;
    const int32_t em_g_q12    = j->em_g_q12;
    const int32_t em_b_q12    = j->em_b_q12;
    const int32_t mat_ar_q12  = j->mat_ar_q12;
    const int32_t mat_ag_q12  = j->mat_ag_q12;
    const int32_t mat_ab_q12  = j->mat_ab_q12;
    const int32_t amb_gr_q12  = j->amb_gr_q12;
    const int32_t amb_gg_q12  = j->amb_gg_q12;
    const int32_t amb_gb_q12  = j->amb_gb_q12;
    const int32_t amb_sr_q12  = j->amb_sr_q12;
    const int32_t amb_sg_q12  = j->amb_sg_q12;
    const int32_t amb_sb_q12  = j->amb_sb_q12;
    const int32_t env_l0x_q14 = j->env_l0x_q14;
    const int32_t env_l0y_q14 = j->env_l0y_q14;
    const int32_t env_l0z_q14 = j->env_l0z_q14;
    const int32_t l0_cr_q12   = j->l0_cr_q12;
    const int32_t l0_cg_q12   = j->l0_cg_q12;
    const int32_t l0_cb_q12   = j->l0_cb_q12;
    const int32_t cam_x_q8    = j->cam_x_q8;
    const int32_t cam_y_q8    = j->cam_y_q8;
    const int32_t cam_z_q8    = j->cam_z_q8;

    static const uint32_t one_u64[2] = { 0u, 0x3FF00000u };

    for (uint32_t ti = 0; ti < j->tri_count; ti++) {
        const g3d_mesh_tri_t *t = &j->tris[ti];
        int sy0 = t->min_y > band_y0 ? t->min_y : band_y0;
        int sy1 = t->max_y < (band_y1 - 1) ? t->max_y : (band_y1 - 1);
        if (sy0 > sy1) continue;

        int min_x = t->min_x, max_x = t->max_x;
        int skip_y = sy0 - t->min_y;

        if (t->use_q12) {
            int32_t iw0_q14 = t->q.iw0_q14, iw1_q14 = t->q.iw1_q14, iw2_q14 = t->q.iw2_q14;
            int32_t dw0_dx_q = t->q.dw0_dx_q, dw1_dx_q = t->q.dw1_dx_q;
            int32_t dw0_dy_q = t->q.dw0_dy_q, dw1_dy_q = t->q.dw1_dy_q;
            int32_t w0_row_q = (int32_t)((int64_t)t->q.w0_row_q + (int64_t)skip_y * dw0_dy_q);
            int32_t w1_row_q = (int32_t)((int64_t)t->q.w1_row_q + (int64_t)skip_y * dw1_dy_q);
            int32_t sz0_q24 = t->q.sz0_q24, sz1_q24 = t->q.sz1_q24, sz2_q24 = t->q.sz2_q24;
            int32_t wx0_q = t->q.wx0_q, wy0_q = t->q.wy0_q, wz0_q = t->q.wz0_q;
            int32_t wx1_q = t->q.wx1_q, wy1_q = t->q.wy1_q, wz1_q = t->q.wz1_q;
            int32_t wx2_q = t->q.wx2_q, wy2_q = t->q.wy2_q, wz2_q = t->q.wz2_q;
            int32_t nx0_q = t->q.nx0_q, ny0_q = t->q.ny0_q, nz0_q = t->q.nz0_q;
            int32_t nx1_q = t->q.nx1_q, ny1_q = t->q.ny1_q, nz1_q = t->q.nz1_q;
            int32_t nx2_q = t->q.nx2_q, ny2_q = t->q.ny2_q, nz2_q = t->q.nz2_q;
            int32_t cr0_q = t->q.cr0_q, cg0_q = t->q.cg0_q, cb0_q = t->q.cb0_q;
            int32_t cr1_q = t->q.cr1_q, cg1_q = t->q.cg1_q, cb1_q = t->q.cb1_q;
            int32_t cr2_q = t->q.cr2_q, cg2_q = t->q.cg2_q, cb2_q = t->q.cb2_q;
            int32_t u0_q = t->q.u0_q, v0_q = t->q.v0_q;
            int32_t u1_q = t->q.u1_q, v1_q = t->q.v1_q;
            int32_t u2_q = t->q.u2_q, v2_q = t->q.v2_q;

            for (int py = sy0; py <= sy1; py++) {
                int32_t bw0 = w0_row_q, bw1 = w1_row_q;
                bool inside_seen = false;
                size_t row_off = (size_t)py * (size_t)w;
                for (int px = min_x; px <= max_x; px++) {
                    int32_t bw2 = 1048576 - bw0 - bw1;
                    if (((bw0 + 105) | (bw1 + 105) | (bw2 + 105)) >= 0) {
                        inside_seen = true;
                        int32_t iwp_q14 = ((bw0 >> 6) * iw0_q14 + (bw1 >> 6) * iw1_q14 + (bw2 >> 6) * iw2_q14) >> 14;
                        if (iwp_q14 > 0) {
                            int32_t z_q24 = (int32_t)(((int64_t)bw0 * sz0_q24 + (int64_t)bw1 * sz1_q24 + (int64_t)bw2 * sz2_q24) >> 20);
                            size_t pidx = row_off + (size_t)px;
                            if (!depth_test || (z_q24 >= 0 && z_q24 <= 16777216)) {
                                uint32_t zbits = g3d_z_q24_to_f32bits(z_q24);
                                uint32_t old_zb;
                                memcpy(&old_zb, &ctx->z_buffer[pidx], 4);
                                if (!depth_test || zbits < old_zb) {
                                    if (depth_write) {
                                        memcpy(&ctx->z_buffer[pidx], &zbits, 4);
                                    }
                                    uint32_t wp_q14 = (1u << 28) / (uint32_t)iwp_q14;
                                    int32_t b0_12 = bw0 >> 8, b1_12 = bw1 >> 8, b2_12 = bw2 >> 8;

                                    int32_t wpx_q8 = (int32_t)(((int64_t)(b0_12 * wx0_q + b1_12 * wx1_q + b2_12 * wx2_q) * (int64_t)wp_q14) >> 26);
                                    int32_t wpy_q8 = (int32_t)(((int64_t)(b0_12 * wy0_q + b1_12 * wy1_q + b2_12 * wy2_q) * (int64_t)wp_q14) >> 26);
                                    int32_t wpz_q8 = (int32_t)(((int64_t)(b0_12 * wz0_q + b1_12 * wz1_q + b2_12 * wz2_q) * (int64_t)wp_q14) >> 26);

                                    int32_t nx_raw = (b0_12 * nx0_q + b1_12 * nx1_q + b2_12 * nx2_q) >> 10;
                                    int32_t ny_raw = (b0_12 * ny0_q + b1_12 * ny1_q + b2_12 * ny2_q) >> 10;
                                    int32_t nz_raw = (b0_12 * nz0_q + b1_12 * nz1_q + b2_12 * nz2_q) >> 10;
                                    int32_t Nx, Ny, Nz;
                                    g3d_norm_q14(nx_raw, ny_raw, nz_raw, &Nx, &Ny, &Nz);

                                    int32_t Vx, Vy, Vz;
                                    g3d_norm_q14(cam_x_q8 - wpx_q8, cam_y_q8 - wpy_q8, cam_z_q8 - wpz_q8, &Vx, &Vy, &Vz);

                                    int32_t ndotv_q14 = (Nx * Vx + Ny * Vy + Nz * Vz) >> 14;
                                    if (ndotv_q14 < 0) ndotv_q14 = 0; else if (ndotv_q14 > 16384) ndotv_q14 = 16384;
                                    int32_t ndotv_q12 = ndotv_q14 >> 2;

                                    int32_t vcr = (int32_t)(((int64_t)(b0_12 * cr0_q + b1_12 * cr1_q + b2_12 * cr2_q) * (int64_t)wp_q14) >> 26);
                                    int32_t vcg = (int32_t)(((int64_t)(b0_12 * cg0_q + b1_12 * cg1_q + b2_12 * cg2_q) * (int64_t)wp_q14) >> 26);
                                    int32_t vcb = (int32_t)(((int64_t)(b0_12 * cb0_q + b1_12 * cb1_q + b2_12 * cb2_q) * (int64_t)wp_q14) >> 26);
                                    int32_t alb_r = (mat_ar_q12 * vcr) >> 12;
                                    int32_t alb_g = (mat_ag_q12 * vcg) >> 12;
                                    int32_t alb_b = (mat_ab_q12 * vcb) >> 12;

                                    if (tex_q12) {
                                        int32_t tu_q16 = (int32_t)(((int64_t)(b0_12 * u0_q + b1_12 * u1_q + b2_12 * u2_q) * (int64_t)wp_q14) >> 22);
                                        int32_t tv_q16 = (int32_t)(((int64_t)(b0_12 * v0_q + b1_12 * v1_q + b2_12 * v2_q) * (int64_t)wp_q14) >> 22);
                                        int32_t tr, tg, tb;
                                        sample_tex_bilinear_q12(tex_q12, tex_w, tex_h, tu_q16, tv_q16, &tr, &tg, &tb);
                                        alb_r = (alb_r * tr) >> 12;
                                        alb_g = (alb_g * tg) >> 12;
                                        alb_b = (alb_b * tb) >> 12;
                                    }

                                    int32_t f5 = (int32_t)s_g3d_fresnel_q12[ndotv_q12];
                                    int32_t f0_r = f0_base_q12 + ((alb_r * metal_q12) >> 12);
                                    int32_t f0_g = f0_base_q12 + ((alb_g * metal_q12) >> 12);
                                    int32_t f0_b = f0_base_q12 + ((alb_b * metal_q12) >> 12);
                                    int32_t fr = f0_r + (((4096 - f0_r) * f5) >> 12);
                                    int32_t fg = f0_g + (((4096 - f0_g) * f5) >> 12);
                                    int32_t fb = f0_b + (((4096 - f0_b) * f5) >> 12);

                                    int32_t hemi = 2048 + (Ny >> 3);
                                    if (hemi < 0) hemi = 0; else if (hemi > 4096) hemi = 4096;
                                    int32_t omh = 4096 - hemi;
                                    int32_t out_r = em_r_q12 + (((((amb_gr_q12 * omh + amb_sr_q12 * hemi) >> 12) * alb_r) >> 12) * amb_w_q12 >> 12);
                                    int32_t out_g = em_g_q12 + (((((amb_gg_q12 * omh + amb_sg_q12 * hemi) >> 12) * alb_g) >> 12) * amb_w_q12 >> 12);
                                    int32_t out_b = em_b_q12 + (((((amb_gb_q12 * omh + amb_sb_q12 * hemi) >> 12) * alb_b) >> 12) * amb_w_q12 >> 12);

                                    if (has_refl) {
                                        int32_t two_nv = ndotv_q14 << 1;
                                        int32_t rx = ((two_nv * Nx) >> 14) - Vx;
                                        int32_t ry = ((two_nv * Ny) >> 14) - Vy;
                                        int32_t rz = ((two_nv * Nz) >> 14) - Vz;
                                        int32_t ty = 2048 + (ry >> 3);
                                        if (ty < 0) ty = 0; else if (ty > 4096) ty = 4096;
                                        int32_t om_ty = 4096 - ty;
                                        int32_t er = (amb_gr_q12 * om_ty + amb_sr_q12 * ty) >> 12;
                                        int32_t eg = (amb_gg_q12 * om_ty + amb_sg_q12 * ty) >> 12;
                                        int32_t eb = (amb_gb_q12 * om_ty + amb_sb_q12 * ty) >> 12;
                                        int32_t ary = ry < 0 ? -ry : ry;
                                        if (ary < 3200) {
                                            int32_t h = ((3200 - ary) * 512) >> 12;
                                            er += (h * 580) >> 9;
                                            eg += (h * 700) >> 9;
                                            eb += (h * 980) >> 9;
                                        }
                                        int32_t arx = rx < 0 ? -rx : rx;
                                        int32_t arz = rz < 0 ? -rz : rz;
                                        if (ry > 8600 && arx < 12000 && arz < 12000) {
                                            int32_t gy = (ry - 8600) >> 4; if (gy > 256) gy = 256;
                                            int32_t gx = (12000 - arx) >> 4; if (gx > 256) gx = 256;
                                            int32_t gz = (12000 - arz) >> 4; if (gz > 256) gz = 256;
                                            int32_t cbox = ((gy * gx) >> 8) * gz >> 8;
                                            if (arx < 560 || arz < 560) cbox >>= 3;
                                            int32_t cg = cbox * 60;
                                            er += cg;
                                            eg += (cg * 3980) >> 12;
                                            eb += (cg * 3780) >> 12;
                                        }
                                        if (rx > 8600 && ry > -2800 && ry < 10800 && arz < 10500) {
                                            int32_t kx = (rx - 8600) >> 4; if (kx > 256) kx = 256;
                                            int32_t ky = (ry < 2000) ? ((ry + 2800) >> 4) : ((10800 - ry) >> 4);
                                            if (ky > 256) ky = 256;
                                            int32_t rk = (kx * ky) >> 8;
                                            er += rk * 38;
                                            eg += rk * 35;
                                            eb += rk * 31;
                                        }
                                        if (rx < -8800 && ry > -3600 && ry < 10200 && arz < 10800) {
                                            int32_t lx = (-8800 - rx) >> 4; if (lx > 256) lx = 256;
                                            int32_t ly = (ry < 2000) ? ((ry + 3600) >> 4) : ((10200 - ry) >> 4);
                                            if (ly > 256) ly = 256;
                                            int32_t lc = (lx * ly) >> 8;
                                            er += lc * 14;
                                            eg += lc * 22;
                                            eb += lc * 38;
                                        }
                                        if (env_has_l0) {
                                            int32_t dq = (rx * env_l0x_q14 + ry * env_l0y_q14 + rz * env_l0z_q14) >> 16;
                                            if (dq > 3072) {
                                                int32_t p = dq - 3072;
                                                int32_t lobe = (((p * p) >> 8) * 7373) >> 12;
                                                er += (l0_cr_q12 * lobe) >> 12;
                                                eg += (l0_cg_q12 * lobe) >> 12;
                                                eb += (l0_cb_q12 * lobe) >> 12;
                                            }
                                        }
                                        out_r += (((er * fr) >> 12) * refl_ao_q12) >> 12;
                                        out_g += (((eg * fg) >> 12) * refl_ao_q12) >> 12;
                                        out_b += (((eb * fb) >> 12) * refl_ao_q12) >> 12;
                                    }

                                    int32_t alb_kd_r = (alb_r * kd_q12) >> 12;
                                    int32_t alb_kd_g = (alb_g * kd_q12) >> 12;
                                    int32_t alb_kd_b = (alb_b * kd_q12) >> 12;

                                    for (int il = 0; il < num_active_lights; il++) {
                                        const g3d_light_pre_t *lt = &pre_lq[il];
                                        int32_t Lx, Ly, Lz;
                                        int32_t att_q12 = 4096;
                                        if (lt->is_point) {
                                            int32_t dx = lt->x - wpx_q8;
                                            int32_t dy = lt->y - wpy_q8;
                                            int32_t dz = lt->z - wpz_q8;
                                            g3d_norm_q14(dx, dy, dz, &Lx, &Ly, &Lz);
                                            int32_t d_q8 = (dx * Lx + dy * Ly + dz * Lz) >> 14;
                                            if (d_q8 < 0) d_q8 = -d_q8;
                                            int32_t d2_q8 = (int32_t)(((int64_t)d_q8 * d_q8) >> 16);
                                            int32_t den_q12 = 4096 + ((d_q8 * 369) >> 8) + d2_q8 * 131;
                                            if (den_q12 < 4096) den_q12 = 4096;
                                            att_q12 = (int32_t)((16777216u) / (uint32_t)den_q12);
                                        } else {
                                            Lx = lt->x; Ly = lt->y; Lz = lt->z;
                                        }
                                        int32_t NdotL = (Nx * Lx + Ny * Ly + Nz * Lz) >> 16;
                                        if (NdotL <= 0) continue;
                                        if (NdotL > 4096) NdotL = 4096;

                                        int32_t Hx, Hy, Hz;
                                        g3d_norm_q14((Lx + Vx) >> 1, (Ly + Vy) >> 1, (Lz + Vz) >> 1, &Hx, &Hy, &Hz);
                                        int32_t NdotH = (Nx * Hx + Ny * Hy + Nz * Hz) >> 16;
                                        int32_t spec = 0;
                                        if (NdotH > 0) {
                                            if (NdotH > 4096) NdotH = 4096;
                                            int gi = NdotH >> 2;
                                            int gf = NdotH & 3;
                                            spec = (int32_t)ggx_lut[gi] + ((((int32_t)ggx_lut[gi + 1] - (int32_t)ggx_lut[gi]) * gf) >> 2);
                                        }

                                        int32_t lr = (lt->cr * att_q12) >> 12;
                                        int32_t lg = (lt->cg * att_q12) >> 12;
                                        int32_t lb = (lt->cb * att_q12) >> 12;
                                        out_r += (int32_t)((((int64_t)alb_kd_r * NdotL + (int64_t)fr * spec) >> 12) * lr >> 12);
                                        out_g += (int32_t)((((int64_t)alb_kd_g * NdotL + (int64_t)fg * spec) >> 12) * lg >> 12);
                                        out_b += (int32_t)((((int64_t)alb_kd_b * NdotL + (int64_t)fb * spec) >> 12) * lb >> 12);
                                    }

                                    if (!is_translucent) {
                                        g3d_write_q12_f64(&ctx->color_fb[pidx].r, out_r);
                                        g3d_write_q12_f64(&ctx->color_fb[pidx].g, out_g);
                                        g3d_write_q12_f64(&ctx->color_fb[pidx].b, out_b);
                                        memcpy(&ctx->color_fb[pidx].a, one_u64, 8);
                                    } else {
                                        double sr = (out_r > 0) ? ((double)out_r * (1.0 / 4096.0)) : 0.0;
                                        double sg = (out_g > 0) ? ((double)out_g * (1.0 / 4096.0)) : 0.0;
                                        double sb = (out_b > 0) ? ((double)out_b * (1.0 / 4096.0)) : 0.0;
                                        double om_a = 1.0 - mat_aa;
                                        ctx->color_fb[pidx].r = sr * mat_aa + ctx->color_fb[pidx].r * om_a;
                                        ctx->color_fb[pidx].g = sg * mat_aa + ctx->color_fb[pidx].g * om_a;
                                        ctx->color_fb[pidx].b = sb * mat_aa + ctx->color_fb[pidx].b * om_a;
                                        ctx->color_fb[pidx].a = 1.0;
                                    }
                                }
                            }
                        }
                    } else if (inside_seen) {
                        break;
                    }
                    bw0 += dw0_dx_q;
                    bw1 += dw1_dx_q;
                }
                w0_row_q += dw0_dy_q;
                w1_row_q += dw1_dy_q;
            }
            continue;
        }

        float iw0 = t->f.iw0, iw1 = t->f.iw1, iw2 = t->f.iw2;
        float sz0 = t->f.sz0, sz1 = t->f.sz1, sz2 = t->f.sz2;
        float dw0_dx = t->f.dw0_dx, dw1_dx = t->f.dw1_dx;
        float dw0_dy = t->f.dw0_dy, dw1_dy = t->f.dw1_dy;
        float w0_row = t->f.w0_row + (float)skip_y * dw0_dy;
        float w1_row = t->f.w1_row + (float)skip_y * dw1_dy;
        float wx0 = t->f.wx0, wy0 = t->f.wy0, wz0 = t->f.wz0;
        float wx1 = t->f.wx1, wy1 = t->f.wy1, wz1 = t->f.wz1;
        float wx2 = t->f.wx2, wy2 = t->f.wy2, wz2 = t->f.wz2;
        float nx0 = t->f.nx0, ny0 = t->f.ny0, nz0 = t->f.nz0;
        float nx1 = t->f.nx1, ny1 = t->f.ny1, nz1 = t->f.nz1;
        float nx2 = t->f.nx2, ny2 = t->f.ny2, nz2 = t->f.nz2;
        float cr0 = t->f.cr0, cg0 = t->f.cg0, cb0 = t->f.cb0;
        float cr1 = t->f.cr1, cg1 = t->f.cg1, cb1 = t->f.cb1;
        float cr2 = t->f.cr2, cg2 = t->f.cg2, cb2 = t->f.cb2;
        float u0 = t->f.u0, v0t = t->f.v0t;
        float u1 = t->f.u1, v1t = t->f.v1t;
        float u2 = t->f.u2, v2t = t->f.v2t;

        float cam_x = j->cam_x, cam_y = j->cam_y, cam_z = j->cam_z;
        float mat_ar = j->mat_ar, mat_ag = j->mat_ag, mat_ab = j->mat_ab;
        float metal = j->metal, kd = j->kd, em_r = j->em_r, em_g = j->em_g, em_b = j->em_b;
        float amb_gr = j->amb_gr, amb_gg = j->amb_gg, amb_gb = j->amb_gb;
        float amb_sr = j->amb_sr, amb_sg = j->amb_sg, amb_sb = j->amb_sb;
        float amb_w = j->amb_w, refl_w = j->refl_w, ao = j->ao;
        float a2_m1 = j->a2_m1, a2_inv_pi = j->a2_inv_pi;
        float env_l0x = j->env_l0x, env_l0y = j->env_l0y, env_l0z = j->env_l0z;

        for (int py = sy0; py <= sy1; py++) {
            float bw0 = w0_row, bw1 = w1_row;
            bool inside_seen = false;
            size_t row_off = (size_t)py * (size_t)w;
            for (int px = min_x; px <= max_x; px++) {
                float bw2 = 1.0f - bw0 - bw1;
                if (bw0 >= -1e-4f && bw1 >= -1e-4f && bw2 >= -1e-4f) {
                    inside_seen = true;
                    float iwp = bw0 * iw0 + bw1 * iw1 + bw2 * iw2;
                    if (iwp > 1e-8f) {
                        float zf = bw0 * sz0 + bw1 * sz1 + bw2 * sz2;
                        size_t pidx = row_off + (size_t)px;
                        if (!depth_test || (zf >= 0.0f && zf <= 1.0f && zf < ctx->z_buffer[pidx])) {
                            if (depth_write) ctx->z_buffer[pidx] = zf;
                            float wp = 1.0f / iwp;

                            float wpx = (bw0 * wx0 + bw1 * wx1 + bw2 * wx2) * wp;
                            float wpy = (bw0 * wy0 + bw1 * wy1 + bw2 * wy2) * wp;
                            float wpz = (bw0 * wz0 + bw1 * wz1 + bw2 * wz2) * wp;

                            float nx_raw = bw0 * nx0 + bw1 * nx1 + bw2 * nx2;
                            float ny_raw = bw0 * ny0 + bw1 * ny1 + bw2 * ny2;
                            float nz_raw = bw0 * nz0 + bw1 * nz1 + bw2 * nz2;
                            float n_l2 = nx_raw * nx_raw + ny_raw * ny_raw + nz_raw * nz_raw;
                            float n_inv = g3d_rsqrtf(n_l2);
                            float Nx = nx_raw * n_inv, Ny = ny_raw * n_inv, Nz = nz_raw * n_inv;

                            float vx_raw = cam_x - wpx, vy_raw = cam_y - wpy, vz_raw = cam_z - wpz;
                            float v_l2 = vx_raw * vx_raw + vy_raw * vy_raw + vz_raw * vz_raw;
                            float v_inv = g3d_rsqrtf(v_l2);
                            float Vx = vx_raw * v_inv, Vy = vy_raw * v_inv, Vz = vz_raw * v_inv;
                            float NdotV = Nx * Vx + Ny * Vy + Nz * Vz;
                            if (NdotV < 0.0f) NdotV = 0.0f;

                            float alb_r = mat_ar * ((bw0 * cr0 + bw1 * cr1 + bw2 * cr2) * wp);
                            float alb_g = mat_ag * ((bw0 * cg0 + bw1 * cg1 + bw2 * cg2) * wp);
                            float alb_b = mat_ab * ((bw0 * cb0 + bw1 * cb1 + bw2 * cb2) * wp);
                            if (albedo_tex) {
                                float tu = (bw0 * u0 + bw1 * u1 + bw2 * u2) * wp;
                                float tv = (bw0 * v0t + bw1 * v1t + bw2 * v2t) * wp;
                                float tr, tg, tb;
                                sample_tex_bilinear_f(albedo_tex, tu, tv, &tr, &tg, &tb);
                                alb_r *= tr; alb_g *= tg; alb_b *= tb;
                            }

                            float f0_r = 0.04f * kd + alb_r * metal;
                            float f0_g = 0.04f * kd + alb_g * metal;
                            float f0_b = 0.04f * kd + alb_b * metal;
                            float om = 1.0f - NdotV;
                            float om2 = om * om;
                            float om5 = om2 * om2 * om;
                            float fr = f0_r + (1.0f - f0_r) * om5;
                            float fg = f0_g + (1.0f - f0_g) * om5;
                            float fb = f0_b + (1.0f - f0_b) * om5;

                            float out_r = em_r, out_g = em_g, out_b = em_b;
                            float hemi = 0.5f * (Ny + 1.0f);
                            float omh = 1.0f - hemi;
                            out_r += (amb_gr * omh + amb_sr * hemi) * alb_r * amb_w;
                            out_g += (amb_gg * omh + amb_sg * hemi) * alb_g * amb_w;
                            out_b += (amb_gb * omh + amb_sb * hemi) * alb_b * amb_w;

                            if (has_refl) {
                                float two_nv = 2.0f * NdotV;
                                float rx = Nx * two_nv - Vx;
                                float ry = Ny * two_nv - Vy;
                                float rz = Nz * two_nv - Vz;
                                float er, eg, eb;
                                sample_env_sky_f(rx, ry, rz, ctx, env_has_l0, env_l0x, env_l0y, env_l0z,
                                                 &er, &eg, &eb);
                                out_r += er * fr * refl_w * ao;
                                out_g += eg * fg * refl_w * ao;
                                out_b += eb * fb * refl_w * ao;
                            }

                            for (int idx_l = 0; idx_l < num_active_lights; idx_l++) {
                                const ipo_gfx3d_light_t *lt = &ctx->lights[active_lights[idx_l]];
                                float Lx, Ly, Lz;
                                float att = 1.0f;
                                if (lt->is_point) {
                                    float dx = (float)lt->pos_or_dir.x - wpx;
                                    float dy = (float)lt->pos_or_dir.y - wpy;
                                    float dz = (float)lt->pos_or_dir.z - wpz;
                                    float d2 = dx * dx + dy * dy + dz * dz;
                                    float dinv = g3d_rsqrtf(d2);
                                    float d = d2 * dinv;
                                    Lx = dx * dinv; Ly = dy * dinv; Lz = dz * dinv;
                                    att = 1.0f / (1.0f + 0.09f * d + 0.032f * d2);
                                } else {
                                    int li = active_lights[idx_l];
                                    Lx = (float)pre_L[li].x; Ly = (float)pre_L[li].y; Lz = (float)pre_L[li].z;
                                }
                                float NdotL = Nx * Lx + Ny * Ly + Nz * Lz;
                                if (NdotL <= 0.0f) continue;

                                float hx = Lx + Vx, hy = Ly + Vy, hz = Lz + Vz;
                                float h2 = hx * hx + hy * hy + hz * hz;
                                float NdotH = (Nx * hx + Ny * hy + Nz * hz) * g3d_rsqrtf(h2);
                                float spec = 0.0f;
                                if (NdotH > 0.0f) {
                                    float ggx_den = (NdotH * NdotH) * a2_m1 + 1.0f;
                                    spec = a2_inv_pi / (ggx_den * ggx_den);
                                }

                                float lr = (float)lt->color.r * att;
                                float lg = (float)lt->color.g * att;
                                float lb = (float)lt->color.b * att;
                                out_r += (alb_r * kd * NdotL + fr * spec) * lr;
                                out_g += (alb_g * kd * NdotL + fg * spec) * lg;
                                out_b += (alb_b * kd * NdotL + fb * spec) * lb;
                            }

                            if (!is_translucent) {
                                ctx->color_fb[pidx].r = (double)out_r;
                                ctx->color_fb[pidx].g = (double)out_g;
                                ctx->color_fb[pidx].b = (double)out_b;
                                ctx->color_fb[pidx].a = 1.0;
                            } else {
                                double om_a = 1.0 - mat_aa;
                                ctx->color_fb[pidx].r = (double)out_r * mat_aa + ctx->color_fb[pidx].r * om_a;
                                ctx->color_fb[pidx].g = (double)out_g * mat_aa + ctx->color_fb[pidx].g * om_a;
                                ctx->color_fb[pidx].b = (double)out_b * mat_aa + ctx->color_fb[pidx].b * om_a;
                                ctx->color_fb[pidx].a = 1.0;
                            }
                        }
                    }
                } else if (inside_seen) {
                    break;
                }
                bw0 += dw0_dx;
                bw1 += dw1_dx;
            }
            w0_row += dw0_dy;
            w1_row += dw1_dy;
        }
    }
}

static inline ipo_mat4_t mat4_normal_matrix3x3(const ipo_mat4_t *m) {
    double a = m->m[0], b = m->m[4], c = m->m[8];
    double d = m->m[1], e = m->m[5], f = m->m[9];
    double g = m->m[2], h = m->m[6], k = m->m[10];
    double c00 = e * k - f * h, c01 = f * g - d * k, c02 = d * h - e * g;
    double c10 = c * h - b * k, c11 = a * k - c * g, c12 = b * g - a * h;
    double c20 = b * f - c * e, c21 = c * d - a * f, c22 = a * e - b * d;
    double det = a * c00 + b * c01 + c * c02;
    if (fabs(det) <= 1e-10) return *m;
    double inv = 1.0 / det;
    ipo_mat4_t nm = ipo_mat4_identity();
    nm.m[0] = c00 * inv; nm.m[4] = c01 * inv; nm.m[8]  = c02 * inv;
    nm.m[1] = c10 * inv; nm.m[5] = c11 * inv; nm.m[9]  = c12 * inv;
    nm.m[2] = c20 * inv; nm.m[6] = c21 * inv; nm.m[10] = c22 * inv;
    return nm;
}

void ipo_gfx3d_draw_mesh(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                         const ipo_mat4_t *model, const ipo_gfx3d_material_t *mat) {
    if (!ctx || !mesh || !mesh->verts || !mesh->indices || !model || !mat) return;
    extern bool g3d_gpu_draw_mesh(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                                  const ipo_mat4_t *model, const ipo_gfx3d_material_t *mat);
    if (g3d_gpu_draw_mesh(ctx, mesh, model, mat)) return;
    g3d_init_luts();

    ipo_mat4_t vp = ipo_mat4_mul(&ctx->proj, &ctx->view);
    ipo_mat4_t mvp = ipo_mat4_mul(&vp, model);
    ipo_mat4_t norm_mat = mat4_normal_matrix3x3(model);
    int w = ctx->width, h = ctx->height;

    float rough = (float)(mat->roughness < 0.04 ? 0.04 : (mat->roughness > 1.0 ? 1.0 : mat->roughness));
    float a2 = (rough * rough) * (rough * rough);
    float a2_m1 = a2 - 1.0f;
    float a2_inv_pi = a2 * 0.31830988618f;
    float metal = (float)(mat->metallic < 0.0 ? 0.0 : (mat->metallic > 1.0 ? 1.0 : mat->metallic));
    float ao = (mat->ao > 0.0) ? (float)mat->ao : 1.0f;
    float kd = 1.0f - metal;
    bool has_refl = ctx->enable_env_reflection && (mat->reflectivity > 0.0 || metal > 0.0 || mat->clearcoat > 0.0);
    float refl_w = (float)mat->reflectivity * (1.0f - rough * 0.6f);
    float amb_w = kd * ao;

    float amb_gr = (float)ctx->ambient_ground.r, amb_gg = (float)ctx->ambient_ground.g, amb_gb = (float)ctx->ambient_ground.b;
    float amb_sr = (float)ctx->ambient_sky.r,    amb_sg = (float)ctx->ambient_sky.g,    amb_sb = (float)ctx->ambient_sky.b;
    float em_r = (float)mat->emissive.r, em_g = (float)mat->emissive.g, em_b = (float)mat->emissive.b;
    float mat_ar = (float)mat->albedo.r, mat_ag = (float)mat->albedo.g, mat_ab = (float)mat->albedo.b;
    double mat_aa = (mat->albedo.a < 0.0) ? 0.0 : (mat->albedo.a > 1.0 ? 1.0 : mat->albedo.a);
    bool is_translucent = (mat_aa < 0.999);
    float cam_x = (float)ctx->cam_pos.x, cam_y = (float)ctx->cam_pos.y, cam_z = (float)ctx->cam_pos.z;

    static ipo_vec3_t      *s_pre_L = NULL;
    static uint32_t        *s_active_lights = NULL;
    static g3d_light_pre_t *s_pre_lq = NULL;
    static uint32_t         s_light_scratch_cap = 0u;

    uint32_t lcap = (ctx->lights && ctx->lights_cap > 0u) ? ctx->lights_cap : 0u;
    if (lcap > s_light_scratch_cap) {
        size_t b_l = 0u, b_a = 0u, b_q = 0u;
        if (ipo_size_mul_ok((size_t)lcap, sizeof(ipo_vec3_t), &b_l) &&
            ipo_size_mul_ok((size_t)lcap, sizeof(uint32_t), &b_a) &&
            ipo_size_mul_ok((size_t)lcap, sizeof(g3d_light_pre_t), &b_q)) {
            ipo_vec3_t      *nl = (ipo_vec3_t *)realloc(s_pre_L, b_l);
            uint32_t        *na = (uint32_t *)realloc(s_active_lights, b_a);
            g3d_light_pre_t *nq = (g3d_light_pre_t *)realloc(s_pre_lq, b_q);
            if (nl) s_pre_L = nl;
            if (na) s_active_lights = na;
            if (nq) s_pre_lq = nq;
            if (nl && na && nq) {
                s_light_scratch_cap = lcap;
            }
        }
    }
    uint32_t max_lights = (lcap <= s_light_scratch_cap) ? lcap : s_light_scratch_cap;
    ipo_vec3_t      *pre_L = s_pre_L;
    uint32_t        *active_lights = s_active_lights;
    g3d_light_pre_t *pre_lq = s_pre_lq;
    int num_active_lights = 0;
    bool lights_in_range = true;
    for (uint32_t li = 0u; li < max_lights; li++) {
        if (ctx->lights[li].enabled) {
            int k = num_active_lights++;
            active_lights[k] = li;
            pre_L[li] = ipo_vec3_norm(ctx->lights[li].pos_or_dir);
            pre_lq[k].is_point = ctx->lights[li].is_point ? 1u : 0u;
            if (ctx->lights[li].is_point) {
                double px = ctx->lights[li].pos_or_dir.x;
                double py = ctx->lights[li].pos_or_dir.y;
                double pz = ctx->lights[li].pos_or_dir.z;
                if (fabs(px) > 120.0 || fabs(py) > 120.0 || fabs(pz) > 120.0) lights_in_range = false;
                pre_lq[k].x = (int32_t)(px * 256.0 + (px >= 0.0 ? 0.5 : -0.5));
                pre_lq[k].y = (int32_t)(py * 256.0 + (py >= 0.0 ? 0.5 : -0.5));
                pre_lq[k].z = (int32_t)(pz * 256.0 + (pz >= 0.0 ? 0.5 : -0.5));
            } else {
                pre_lq[k].x = (int32_t)(pre_L[li].x * 16384.0 + (pre_L[li].x >= 0.0 ? 0.5 : -0.5));
                pre_lq[k].y = (int32_t)(pre_L[li].y * 16384.0 + (pre_L[li].y >= 0.0 ? 0.5 : -0.5));
                pre_lq[k].z = (int32_t)(pre_L[li].z * 16384.0 + (pre_L[li].z >= 0.0 ? 0.5 : -0.5));
            }
            if (ctx->lights[li].color.r < 0.0 || ctx->lights[li].color.r > 15.9 ||
                ctx->lights[li].color.g < 0.0 || ctx->lights[li].color.g > 15.9 ||
                ctx->lights[li].color.b < 0.0 || ctx->lights[li].color.b > 15.9) {
                lights_in_range = false;
            }
            pre_lq[k].cr = (int32_t)(ctx->lights[li].color.r * 4096.0 + 0.5);
            pre_lq[k].cg = (int32_t)(ctx->lights[li].color.g * 4096.0 + 0.5);
            pre_lq[k].cb = (int32_t)(ctx->lights[li].color.b * 4096.0 + 0.5);
        } else {
            pre_L[li] = ipo_vec3(0.0, 1.0, 0.0);
        }
    }
    bool env_has_l0 = (max_lights > 0u) && ctx->lights[0].enabled;
    float env_l0x = env_has_l0 ? (float)pre_L[0].x : 0.0f;
    float env_l0y = env_has_l0 ? (float)pre_L[0].y : 1.0f;
    float env_l0z = env_has_l0 ? (float)pre_L[0].z : 0.0f;

    const int16_t *tex_q12 = mat->albedo_tex ? g3d_get_tex_q12(mat->albedo_tex) : NULL;
    int tex_w = mat->albedo_tex ? mat->albedo_tex->width : 0;
    int tex_h = mat->albedo_tex ? mat->albedo_tex->height : 0;
    const uint32_t *ggx_lut = g3d_get_ggx_lut(rough);

    bool use_fast_q12 = (ggx_lut != NULL) && lights_in_range && (!mat->albedo_tex || tex_q12 != NULL) &&
                        fabsf(cam_x) <= 120.0f && fabsf(cam_y) <= 120.0f && fabsf(cam_z) <= 120.0f &&
                        mat_ar >= 0.0f && mat_ar <= 7.9f && mat_ag >= 0.0f && mat_ag <= 7.9f &&
                        mat_ab >= 0.0f && mat_ab <= 7.9f && em_r >= 0.0f && em_g >= 0.0f && em_b >= 0.0f;

    static g3d_xform_v_t *s_xf = NULL;
    static uint32_t s_xf_cap = 0u;
    if (mesh->vert_count > s_xf_cap) {
        g3d_xform_v_t *nb = (g3d_xform_v_t *)realloc(s_xf, (size_t)mesh->vert_count * sizeof(g3d_xform_v_t));
        if (nb) {
            s_xf = nb;
            s_xf_cap = mesh->vert_count;
        }
    }
    bool use_xf_cache = (s_xf && s_xf_cap >= mesh->vert_count);
    if (use_xf_cache) {
        g3d_xform_job_t xf_job = { mesh, &mvp, model, &norm_mat, s_xf, (float)w, (float)h };
        if (mesh->vert_count >= 512u && mesh->vert_count <= 2000000u && s_g3d_pool.worker_count > 0) {
            g3d_pool_dispatch(g3d_xform_verts_worker, &xf_job);
        } else {
            g3d_xform_verts_worker(0, 1, &xf_job);
        }
    }

    uint32_t total_tris = mesh->index_count / 3u;
    static g3d_mesh_tri_t *s_tris = NULL;
    static uint32_t s_tris_cap = 0u;
    if (total_tris > s_tris_cap) {
        size_t bytes = 0u;
        if (ipo_size_mul_ok((size_t)total_tris, sizeof(g3d_mesh_tri_t), &bytes)) {
            g3d_mesh_tri_t *nt = (g3d_mesh_tri_t *)realloc(s_tris, bytes);
            if (nt) {
                s_tris = nt;
                s_tris_cap = total_tris;
            }
        }
    }

    g3d_mesh_tri_t fallback_tri;
    g3d_mesh_tri_t *tri_buf = (s_tris && s_tris_cap > 0u) ? s_tris : &fallback_tri;
    uint32_t tri_cap = (s_tris && s_tris_cap > 0u) ? s_tris_cap : 1u;

    g3d_mesh_job_t job;
    job.ctx = ctx;
    job.w = w;
    job.h = h;
    job.tris = tri_buf;
    job.tri_count = 0u;
    job.a2_m1 = a2_m1;
    job.a2_inv_pi = a2_inv_pi;
    job.metal = metal;
    job.ao = ao;
    job.kd = kd;
    job.refl_w = refl_w;
    job.amb_w = amb_w;
    job.has_refl = has_refl;
    job.is_translucent = is_translucent;
    job.env_has_l0 = env_has_l0;
    job.amb_gr = amb_gr; job.amb_gg = amb_gg; job.amb_gb = amb_gb;
    job.amb_sr = amb_sr; job.amb_sg = amb_sg; job.amb_sb = amb_sb;
    job.em_r = em_r; job.em_g = em_g; job.em_b = em_b;
    job.mat_ar = mat_ar; job.mat_ag = mat_ag; job.mat_ab = mat_ab;
    job.mat_aa = mat_aa;
    job.cam_x = cam_x; job.cam_y = cam_y; job.cam_z = cam_z;
    job.env_l0x = env_l0x; job.env_l0y = env_l0y; job.env_l0z = env_l0z;
    job.albedo_tex = mat->albedo_tex;
    job.tex_q12 = tex_q12;
    job.tex_w = tex_w;
    job.tex_h = tex_h;
    job.ggx_lut = ggx_lut;
    job.pre_L = pre_L;
    job.active_lights = active_lights;
    job.pre_lq = pre_lq;
    job.num_active_lights = num_active_lights;
    job.metal_q12   = (int32_t)(metal * 4096.0f + 0.5f);
    job.kd_q12      = (int32_t)(kd * 4096.0f + 0.5f);
    job.f0_base_q12 = (int32_t)(0.04f * kd * 4096.0f + 0.5f);
    job.amb_w_q12   = (int32_t)(amb_w * 4096.0f + 0.5f);
    job.refl_ao_q12 = (int32_t)(refl_w * ao * 4096.0f + 0.5f);
    job.em_r_q12    = (int32_t)(em_r * 4096.0f + 0.5f);
    job.em_g_q12    = (int32_t)(em_g * 4096.0f + 0.5f);
    job.em_b_q12    = (int32_t)(em_b * 4096.0f + 0.5f);
    job.mat_ar_q12  = (int32_t)(mat_ar * 4096.0f + 0.5f);
    job.mat_ag_q12  = (int32_t)(mat_ag * 4096.0f + 0.5f);
    job.mat_ab_q12  = (int32_t)(mat_ab * 4096.0f + 0.5f);
    job.amb_gr_q12  = (int32_t)(amb_gr * 4096.0f + 0.5f);
    job.amb_gg_q12  = (int32_t)(amb_gg * 4096.0f + 0.5f);
    job.amb_gb_q12  = (int32_t)(amb_gb * 4096.0f + 0.5f);
    job.amb_sr_q12  = (int32_t)(amb_sr * 4096.0f + 0.5f);
    job.amb_sg_q12  = (int32_t)(amb_sg * 4096.0f + 0.5f);
    job.amb_sb_q12  = (int32_t)(amb_sb * 4096.0f + 0.5f);
    job.env_l0x_q14 = (int32_t)(env_l0x * 16384.0f + (env_l0x >= 0.0f ? 0.5f : -0.5f));
    job.env_l0y_q14 = (int32_t)(env_l0y * 16384.0f + (env_l0y >= 0.0f ? 0.5f : -0.5f));
    job.env_l0z_q14 = (int32_t)(env_l0z * 16384.0f + (env_l0z >= 0.0f ? 0.5f : -0.5f));
    job.l0_cr_q12   = env_has_l0 ? (int32_t)(ctx->lights[0].color.r * 4096.0 + 0.5) : 0;
    job.l0_cg_q12   = env_has_l0 ? (int32_t)(ctx->lights[0].color.g * 4096.0 + 0.5) : 0;
    job.l0_cb_q12   = env_has_l0 ? (int32_t)(ctx->lights[0].color.b * 4096.0 + 0.5) : 0;
    job.cam_x_q8    = (int32_t)(cam_x * 256.0f + (cam_x >= 0.0f ? 0.5f : -0.5f));
    job.cam_y_q8    = (int32_t)(cam_y * 256.0f + (cam_y >= 0.0f ? 0.5f : -0.5f));
    job.cam_z_q8    = (int32_t)(cam_z * 256.0f + (cam_z >= 0.0f ? 0.5f : -0.5f));

    uint32_t vis_count = 0u;
    uint64_t bbox_px = 0u;

    for (uint32_t ti = 0; ti + 2u < mesh->index_count; ti += 3u) {
        ctx->tris_submitted++;
        uint32_t i0 = mesh->indices[ti];
        uint32_t i1 = mesh->indices[ti + 1u];
        uint32_t i2 = mesh->indices[ti + 2u];

        g3d_xform_v_t tv0, tv1, tv2;
        const g3d_xform_v_t *xv0, *xv1, *xv2;
        if (use_xf_cache) {
            xv0 = &s_xf[i0]; xv1 = &s_xf[i1]; xv2 = &s_xf[i2];
            if (!xv0->valid || !xv1->valid || !xv2->valid) continue;
        } else {
            const ipo_gfx3d_vertex_t *vs[3] = { &mesh->verts[i0], &mesh->verts[i1], &mesh->verts[i2] };
            g3d_xform_v_t *tvs[3] = { &tv0, &tv1, &tv2 };
            bool ok = true;
            for (int k = 0; k < 3; k++) {
                const ipo_gfx3d_vertex_t *v = vs[k];
                ipo_vec4_t c = mat4_mul_vec4(&mvp, (ipo_vec4_t){v->pos.x, v->pos.y, v->pos.z, 1.0});
                if (c.w <= 0.05) { ok = false; break; }
                float iw = (float)(1.0 / c.w);
                g3d_xform_v_t *o = tvs[k];
                o->iw = iw;
                o->sx = ((float)c.x * iw * 0.5f + 0.5f) * (float)w;
                o->sy = (0.5f - (float)c.y * iw * 0.5f) * (float)h;
                o->sz = ((float)c.z * iw) * 0.5f + 0.5f;
                ipo_vec4_t wp = mat4_mul_vec4(model, (ipo_vec4_t){v->pos.x, v->pos.y, v->pos.z, 1.0});
                ipo_vec3_t wn = ipo_vec3_norm(mat4_mul_dir3(&norm_mat, v->normal));
                o->wx = (float)wp.x; o->wy = (float)wp.y; o->wz = (float)wp.z;
                o->nx = (float)wn.x; o->ny = (float)wn.y; o->nz = (float)wn.z;
                o->cr = (float)v->color.r; o->cg = (float)v->color.g; o->cb = (float)v->color.b;
                o->u = (float)v->u; o->v = (float)v->v;
            }
            if (!ok) continue;
            xv0 = &tv0; xv1 = &tv1; xv2 = &tv2;
        }

        float sx0 = xv0->sx, sy0 = xv0->sy, sz0 = xv0->sz, iw0 = xv0->iw;
        float sx1 = xv1->sx, sy1 = xv1->sy, sz1 = xv1->sz, iw1 = xv1->iw;
        float sx2 = xv2->sx, sy2 = xv2->sy, sz2 = xv2->sz, iw2 = xv2->iw;

        float area = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
        if (fabsf(area) < 1e-6f) continue;
        /* Screen Y is flipped relative to GL up; CCW front faces have area < 0 */
        if (ctx->cull_mode == IPO_GFX3D_CULL_BACK && area > 0.0f) continue;
        if (ctx->cull_mode == IPO_GFX3D_CULL_FRONT && area < 0.0f) continue;
        float inv_area = 1.0f / area;

        float fmin_x = sx0 < sx1 ? (sx0 < sx2 ? sx0 : sx2) : (sx1 < sx2 ? sx1 : sx2);
        float fmax_x = sx0 > sx1 ? (sx0 > sx2 ? sx0 : sx2) : (sx1 > sx2 ? sx1 : sx2);
        float fmin_y = sy0 < sy1 ? (sy0 < sy2 ? sy0 : sy2) : (sy1 < sy2 ? sy1 : sy2);
        float fmax_y = sy0 > sy1 ? (sy0 > sy2 ? sy0 : sy2) : (sy1 > sy2 ? sy1 : sy2);
        int min_x = g3d_clamp_floor(fmin_x, 0, w - 1);
        int max_x = g3d_clamp_ceil(fmax_x, 0, w - 1);
        int min_y = g3d_clamp_floor(fmin_y, 0, h - 1);
        int max_y = g3d_clamp_ceil(fmax_y, 0, h - 1);
        if (min_x > max_x || min_y > max_y) continue;

        ctx->tris_drawn++;

        float dw0_dx = (sy1 - sy2) * inv_area, dw1_dx = (sy2 - sy0) * inv_area;
        float dw0_dy = (sx2 - sx1) * inv_area, dw1_dy = (sx0 - sx2) * inv_area;
        float fx0 = (float)min_x + 0.5f, fy0 = (float)min_y + 0.5f;
        float w0_row = ((sx1 - fx0) * (sy2 - fy0) - (sy1 - fy0) * (sx2 - fx0)) * inv_area;
        float w1_row = ((sx2 - fx0) * (sy0 - fy0) - (sy2 - fy0) * (sx0 - fx0)) * inv_area;

        float iw_max = iw0 > iw1 ? (iw0 > iw2 ? iw0 : iw2) : (iw1 > iw2 ? iw1 : iw2);
        float span_x = (float)(max_x - min_x);
        float span_y = (float)(max_y - min_y);
        float w0_c1 = w0_row + dw0_dx * span_x, w0_c2 = w0_row + dw0_dy * span_y, w0_c3 = w0_c1 + dw0_dy * span_y;
        float w1_c1 = w1_row + dw1_dx * span_x, w1_c2 = w1_row + dw1_dy * span_y, w1_c3 = w1_c1 + dw1_dy * span_y;
        bool bw_in_range = fabsf(w0_row) < 900.0f && fabsf(w0_c1) < 900.0f && fabsf(w0_c2) < 900.0f && fabsf(w0_c3) < 900.0f &&
                           fabsf(w1_row) < 900.0f && fabsf(w1_c1) < 900.0f && fabsf(w1_c2) < 900.0f && fabsf(w1_c3) < 900.0f;
        bool attrs_in_range = iw_max > 1e-8f && sz0 >= -10.0f && sz0 <= 10.0f && sz1 >= -10.0f && sz1 <= 10.0f && sz2 >= -10.0f && sz2 <= 10.0f &&
                              fabsf(xv0->wx) <= 120.0f && fabsf(xv0->wy) <= 120.0f && fabsf(xv0->wz) <= 120.0f &&
                              fabsf(xv1->wx) <= 120.0f && fabsf(xv1->wy) <= 120.0f && fabsf(xv1->wz) <= 120.0f &&
                              fabsf(xv2->wx) <= 120.0f && fabsf(xv2->wy) <= 120.0f && fabsf(xv2->wz) <= 120.0f &&
                              fabsf(xv0->u) <= 64.0f && fabsf(xv0->v) <= 64.0f &&
                              fabsf(xv1->u) <= 64.0f && fabsf(xv1->v) <= 64.0f &&
                              fabsf(xv2->u) <= 64.0f && fabsf(xv2->v) <= 64.0f;

        g3d_mesh_tri_t *dst = &tri_buf[vis_count++];
        dst->min_x = min_x; dst->max_x = max_x;
        dst->min_y = min_y; dst->max_y = max_y;
        bbox_px += (uint64_t)(max_x - min_x + 1) * (uint64_t)(max_y - min_y + 1);

        if (use_fast_q12 && bw_in_range && attrs_in_range) {
            float inv_iwmax = 1.0f / iw_max;
            float ri0 = iw0 * inv_iwmax, ri1 = iw1 * inv_iwmax, ri2 = iw2 * inv_iwmax;
            dst->use_q12 = 1u;
            dst->q.iw0_q14 = (int32_t)(ri0 * 16384.0f + 0.5f);
            dst->q.iw1_q14 = (int32_t)(ri1 * 16384.0f + 0.5f);
            dst->q.iw2_q14 = (int32_t)(ri2 * 16384.0f + 0.5f);

            dst->q.w0_row_q = (int32_t)(w0_row * 1048576.0f);
            dst->q.w1_row_q = (int32_t)(w1_row * 1048576.0f);
            dst->q.dw0_dx_q = (int32_t)(dw0_dx * 1048576.0f);
            dst->q.dw1_dx_q = (int32_t)(dw1_dx * 1048576.0f);
            dst->q.dw0_dy_q = (int32_t)(dw0_dy * 1048576.0f);
            dst->q.dw1_dy_q = (int32_t)(dw1_dy * 1048576.0f);

            dst->q.sz0_q24 = (int32_t)(sz0 * 16777216.0f + 0.5f);
            dst->q.sz1_q24 = (int32_t)(sz1 * 16777216.0f + 0.5f);
            dst->q.sz2_q24 = (int32_t)(sz2 * 16777216.0f + 0.5f);

            dst->q.wx0_q = (int32_t)(xv0->wx * ri0 * 256.0f); dst->q.wy0_q = (int32_t)(xv0->wy * ri0 * 256.0f); dst->q.wz0_q = (int32_t)(xv0->wz * ri0 * 256.0f);
            dst->q.wx1_q = (int32_t)(xv1->wx * ri1 * 256.0f); dst->q.wy1_q = (int32_t)(xv1->wy * ri1 * 256.0f); dst->q.wz1_q = (int32_t)(xv1->wz * ri1 * 256.0f);
            dst->q.wx2_q = (int32_t)(xv2->wx * ri2 * 256.0f); dst->q.wy2_q = (int32_t)(xv2->wy * ri2 * 256.0f); dst->q.wz2_q = (int32_t)(xv2->wz * ri2 * 256.0f);

            dst->q.nx0_q = (int32_t)(xv0->nx * ri0 * 16384.0f); dst->q.ny0_q = (int32_t)(xv0->ny * ri0 * 16384.0f); dst->q.nz0_q = (int32_t)(xv0->nz * ri0 * 16384.0f);
            dst->q.nx1_q = (int32_t)(xv1->nx * ri1 * 16384.0f); dst->q.ny1_q = (int32_t)(xv1->ny * ri1 * 16384.0f); dst->q.nz1_q = (int32_t)(xv1->nz * ri1 * 16384.0f);
            dst->q.nx2_q = (int32_t)(xv2->nx * ri2 * 16384.0f); dst->q.ny2_q = (int32_t)(xv2->ny * ri2 * 16384.0f); dst->q.nz2_q = (int32_t)(xv2->nz * ri2 * 16384.0f);

            dst->q.cr0_q = (int32_t)(xv0->cr * ri0 * 4096.0f); dst->q.cg0_q = (int32_t)(xv0->cg * ri0 * 4096.0f); dst->q.cb0_q = (int32_t)(xv0->cb * ri0 * 4096.0f);
            dst->q.cr1_q = (int32_t)(xv1->cr * ri1 * 4096.0f); dst->q.cg1_q = (int32_t)(xv1->cg * ri1 * 4096.0f); dst->q.cb1_q = (int32_t)(xv1->cb * ri1 * 4096.0f);
            dst->q.cr2_q = (int32_t)(xv2->cr * ri2 * 4096.0f); dst->q.cg2_q = (int32_t)(xv2->cg * ri2 * 4096.0f); dst->q.cb2_q = (int32_t)(xv2->cb * ri2 * 4096.0f);

            dst->q.u0_q = (int32_t)(xv0->u * ri0 * 4096.0f); dst->q.v0_q = (int32_t)(xv0->v * ri0 * 4096.0f);
            dst->q.u1_q = (int32_t)(xv1->u * ri1 * 4096.0f); dst->q.v1_q = (int32_t)(xv1->v * ri1 * 4096.0f);
            dst->q.u2_q = (int32_t)(xv2->u * ri2 * 4096.0f); dst->q.v2_q = (int32_t)(xv2->v * ri2 * 4096.0f);
        } else {
            dst->use_q12 = 0u;
            dst->f.iw0 = iw0; dst->f.iw1 = iw1; dst->f.iw2 = iw2;
            dst->f.sz0 = sz0; dst->f.sz1 = sz1; dst->f.sz2 = sz2;
            dst->f.w0_row = w0_row; dst->f.w1_row = w1_row;
            dst->f.dw0_dx = dw0_dx; dst->f.dw1_dx = dw1_dx;
            dst->f.dw0_dy = dw0_dy; dst->f.dw1_dy = dw1_dy;

            dst->f.wx0 = xv0->wx * iw0; dst->f.wy0 = xv0->wy * iw0; dst->f.wz0 = xv0->wz * iw0;
            dst->f.wx1 = xv1->wx * iw1; dst->f.wy1 = xv1->wy * iw1; dst->f.wz1 = xv1->wz * iw1;
            dst->f.wx2 = xv2->wx * iw2; dst->f.wy2 = xv2->wy * iw2; dst->f.wz2 = xv2->wz * iw2;

            dst->f.nx0 = xv0->nx * iw0; dst->f.ny0 = xv0->ny * iw0; dst->f.nz0 = xv0->nz * iw0;
            dst->f.nx1 = xv1->nx * iw1; dst->f.ny1 = xv1->ny * iw1; dst->f.nz1 = xv1->nz * iw1;
            dst->f.nx2 = xv2->nx * iw2; dst->f.ny2 = xv2->ny * iw2; dst->f.nz2 = xv2->nz * iw2;

            dst->f.cr0 = xv0->cr * iw0; dst->f.cg0 = xv0->cg * iw0; dst->f.cb0 = xv0->cb * iw0;
            dst->f.cr1 = xv1->cr * iw1; dst->f.cg1 = xv1->cg * iw1; dst->f.cb1 = xv1->cb * iw1;
            dst->f.cr2 = xv2->cr * iw2; dst->f.cg2 = xv2->cg * iw2; dst->f.cb2 = xv2->cb * iw2;

            dst->f.u0 = xv0->u * iw0; dst->f.v0t = xv0->v * iw0;
            dst->f.u1 = xv1->u * iw1; dst->f.v1t = xv1->v * iw1;
            dst->f.u2 = xv2->u * iw2; dst->f.v2t = xv2->v * iw2;
        }

        if (vis_count >= tri_cap) {
            job.tri_count = vis_count;
            if (bbox_px >= 4096u && s_g3d_pool.worker_count > 0) {
                g3d_pool_dispatch(g3d_mesh_band_worker, &job);
            } else {
                g3d_mesh_band_worker(0, 1, &job);
            }
            vis_count = 0u;
            bbox_px = 0u;
        }
    }

    if (vis_count > 0u) {
        job.tri_count = vis_count;
        if (bbox_px >= 4096u && s_g3d_pool.worker_count > 0) {
            g3d_pool_dispatch(g3d_mesh_band_worker, &job);
        } else {
            g3d_mesh_band_worker(0, 1, &job);
        }
    }
}

void ipo_gfx3d_draw_mirrored_mesh(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                                  const ipo_mat4_t *model, const ipo_gfx3d_material_t *mat,
                                  double plane_y, double reflectivity_tint) {
    if (!ctx || !mesh || !model || !mat) return;
    ipo_mat4_t tr = ipo_mat4_translate(0.0, 2.0 * plane_y, 0.0);
    ipo_mat4_t sc = ipo_mat4_scale(1.0, -1.0, 1.0);
    ipo_mat4_t refl = ipo_mat4_mul(&tr, &sc);
    ipo_mat4_t mirr_model = ipo_mat4_mul(&refl, model);

    ipo_gfx3d_material_t rmat = *mat;
    double tint = (reflectivity_tint < 0.0) ? 0.0 : reflectivity_tint;
    rmat.albedo.r *= tint;
    rmat.albedo.g *= tint;
    rmat.albedo.b *= tint;
    rmat.albedo.a = 1.0;
    rmat.emissive.r *= tint;
    rmat.emissive.g *= tint;
    rmat.emissive.b *= tint;
    rmat.reflectivity *= tint;
    rmat.clearcoat *= tint;

    ipo_gfx3d_cull_t saved_cull = ctx->cull_mode;
    if (saved_cull == IPO_GFX3D_CULL_BACK)       ctx->cull_mode = IPO_GFX3D_CULL_FRONT;
    else if (saved_cull == IPO_GFX3D_CULL_FRONT) ctx->cull_mode = IPO_GFX3D_CULL_BACK;

    ipo_gfx3d_draw_mesh(ctx, mesh, &mirr_model, &rmat);
    ctx->cull_mode = saved_cull;
}

typedef struct { float sx, sy, sz; uint8_t valid; } g3d_sh_v_t;

typedef struct {
    const ipo_gfx3d_mesh_t *mesh;
    const ipo_mat4_t       *mvp;
    g3d_sh_v_t             *shv;
    float                   wf, hf;
} g3d_shv_job_t;

static void g3d_shv_verts_worker(int tid, int nthreads, void *opaque) {
    const g3d_shv_job_t *j = (const g3d_shv_job_t *)opaque;
    int v0 = 0, v1 = 0;
    g3d_band_range(tid, nthreads, (int)j->mesh->vert_count, &v0, &v1);
    float wf = j->wf, hf = j->hf;
    for (int vi = v0; vi < v1; vi++) {
        const ipo_gfx3d_vertex_t *v = &j->mesh->verts[vi];
        ipo_vec4_t c = mat4_mul_vec4(j->mvp, (ipo_vec4_t){v->pos.x, v->pos.y, v->pos.z, 1.0});
        g3d_sh_v_t *o = &j->shv[vi];
        if (c.w <= 0.05) {
            o->valid = 0u;
            continue;
        }
        float iw = (float)(1.0 / c.w);
        o->valid = 1u;
        o->sx = ((float)c.x * iw * 0.5f + 0.5f) * wf;
        o->sy = (0.5f - (float)c.y * iw * 0.5f) * hf;
        o->sz = ((float)c.z * iw) * 0.5f + 0.5f;
    }
}

typedef struct {
    int min_x, max_x, min_y, max_y;
    uint8_t sh_fast;
    union {
        struct {
            int32_t b0_rq, b1_rq, db0_xq, db1_xq, db0_yq, db1_yq;
            int32_t sz0_q, sz1_q, sz2_q;
        } q;
        struct {
            float sz0, sz1, sz2;
            float b0_row, b1_row, db0_dx, db1_dx, db0_dy, db1_dy;
        } f;
    };
} g3d_shadow_tri_t;

typedef struct {
    ipo_gfx3d_ctx_t        *ctx;
    int                     w, h;
    const g3d_shadow_tri_t *tris;
    uint32_t                tri_count;
    uint8_t                *shd_stamp;
    uint8_t                 tap_id;
    double                  shade;
} g3d_shadow_job_t;

static void g3d_shadow_band_worker(int tid, int nthreads, void *opaque) {
    const g3d_shadow_job_t *j = (const g3d_shadow_job_t *)opaque;
    int band_y0 = 0, band_y1 = 0;
    g3d_band_range(tid, nthreads, j->h, &band_y0, &band_y1);
    if (band_y0 >= band_y1) return;

    ipo_gfx3d_ctx_t *ctx = j->ctx;
    int w = j->w;
    uint8_t *shd_stamp = j->shd_stamp;
    uint8_t tap_id = j->tap_id;
    double shade = j->shade;

    for (uint32_t ti = 0; ti < j->tri_count; ti++) {
        const g3d_shadow_tri_t *t = &j->tris[ti];
        int sy0 = t->min_y > band_y0 ? t->min_y : band_y0;
        int sy1 = t->max_y < (band_y1 - 1) ? t->max_y : (band_y1 - 1);
        if (sy0 > sy1) continue;

        int min_x = t->min_x, max_x = t->max_x;
        int skip_y = sy0 - t->min_y;

        if (t->sh_fast) {
            int32_t db0_xq = t->q.db0_xq, db1_xq = t->q.db1_xq;
            int32_t db0_yq = t->q.db0_yq, db1_yq = t->q.db1_yq;
            int32_t b0_rq = (int32_t)((int64_t)t->q.b0_rq + (int64_t)skip_y * db0_yq);
            int32_t b1_rq = (int32_t)((int64_t)t->q.b1_rq + (int64_t)skip_y * db1_yq);
            int32_t sz0_q = t->q.sz0_q, sz1_q = t->q.sz1_q, sz2_q = t->q.sz2_q;

            for (int py = sy0; py <= sy1; py++) {
                int32_t b0 = b0_rq, b1 = b1_rq;
                bool inside_seen = false;
                size_t row_off = (size_t)py * (size_t)w;
                for (int px = min_x; px <= max_x; px++) {
                    int32_t b2 = 1048576 - b0 - b1;
                    if ((b0 | b1 | b2) >= 0) {
                        inside_seen = true;
                        int32_t z_q24 = (int32_t)(((int64_t)b0 * sz0_q + (int64_t)b1 * sz1_q + (int64_t)b2 * sz2_q) >> 20);
                        if (z_q24 >= 0) {
                            size_t idx = row_off + (size_t)px;
                            uint32_t zbits = g3d_z_q24_to_f32bits(z_q24);
                            uint32_t old_zb;
                            memcpy(&old_zb, &ctx->z_buffer[idx], 4);
                            if (zbits <= old_zb && (!shd_stamp || shd_stamp[idx] != tap_id)) {
                                if (shd_stamp) shd_stamp[idx] = tap_id;
                                ctx->color_fb[idx].r *= shade;
                                ctx->color_fb[idx].g *= shade;
                                ctx->color_fb[idx].b *= shade;
                            }
                        }
                    } else if (inside_seen) {
                        break;
                    }
                    b0 += db0_xq;
                    b1 += db1_xq;
                }
                b0_rq += db0_yq;
                b1_rq += db1_yq;
            }
            continue;
        }

        float sz0 = t->f.sz0, sz1 = t->f.sz1, sz2 = t->f.sz2;
        float db0_dx = t->f.db0_dx, db1_dx = t->f.db1_dx;
        float db0_dy = t->f.db0_dy, db1_dy = t->f.db1_dy;
        float b0_row = t->f.b0_row + (float)skip_y * db0_dy;
        float b1_row = t->f.b1_row + (float)skip_y * db1_dy;

        for (int py = sy0; py <= sy1; py++) {
            float b0 = b0_row, b1 = b1_row;
            bool inside_seen = false;
            size_t row_off = (size_t)py * (size_t)w;
            for (int px = min_x; px <= max_x; px++) {
                float b2 = 1.0f - b0 - b1;
                if (b0 >= 0.0f && b1 >= 0.0f && b2 >= 0.0f) {
                    inside_seen = true;
                    float zf = b0 * sz0 + b1 * sz1 + b2 * sz2;
                    size_t idx = row_off + (size_t)px;
                    if (zf <= ctx->z_buffer[idx] && (!shd_stamp || shd_stamp[idx] != tap_id)) {
                        if (shd_stamp) shd_stamp[idx] = tap_id;
                        ctx->color_fb[idx].r *= shade;
                        ctx->color_fb[idx].g *= shade;
                        ctx->color_fb[idx].b *= shade;
                    }
                } else if (inside_seen) {
                    break;
                }
                b0 += db0_dx;
                b1 += db1_dx;
            }
            b0_row += db0_dy;
            b1_row += db1_dy;
        }
    }
}

void ipo_gfx3d_draw_shadow_projection(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                                      const ipo_mat4_t *model, ipo_vec3_t light_dir,
                                      double plane_y, double opacity) {
    if (!ctx || !mesh || !mesh->verts || !mesh->indices || !model || light_dir.y <= 0.05) return;
    extern bool g3d_gpu_draw_shadow_projection(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                                               const ipo_mat4_t *model, ipo_vec3_t light_dir,
                                               double plane_y, double opacity);
    if (g3d_gpu_draw_shadow_projection(ctx, mesh, model, light_dir, plane_y, opacity)) return;
    if (!ctx->color_fb || !ctx->z_buffer || ctx->width <= 0 || ctx->height <= 0) return;

    int w = ctx->width, h = ctx->height;
    float wf = (float)w, hf = (float)h;
    double clamped_op = (opacity < 0.0) ? 0.0 : (opacity > 0.95 ? 0.95 : opacity);
    double shade = pow(1.0 - clamped_op, 1.0 / 3.0);

    static const double k_shd_ox[3] = { 0.0,  0.045, -0.045 };
    static const double k_shd_oz[3] = { -0.038, 0.032, 0.032 };
    static const double k_shd_yo[3] = { 0.0020, 0.0050, 0.0080 };

    static g3d_sh_v_t *s_shv = NULL;
    static uint32_t s_shv_cap = 0u;
    if (mesh->vert_count > s_shv_cap) {
        g3d_sh_v_t *nb = (g3d_sh_v_t *)realloc(s_shv, (size_t)mesh->vert_count * sizeof(g3d_sh_v_t));
        if (nb) {
            s_shv = nb;
            s_shv_cap = mesh->vert_count;
        }
    }
    bool use_cache = (s_shv && s_shv_cap >= mesh->vert_count);

    static uint8_t *s_shd_stamp = NULL;
    static size_t s_shd_stamp_cap = 0u;
    size_t px_count = (size_t)w * (size_t)h;
    if (px_count > s_shd_stamp_cap) {
        uint8_t *ns = (uint8_t *)realloc(s_shd_stamp, px_count);
        if (ns) {
            s_shd_stamp = ns;
            s_shd_stamp_cap = px_count;
        }
    }
    if (s_shd_stamp && s_shd_stamp_cap >= px_count) {
        memset(s_shd_stamp, 0, px_count);
    }

    uint32_t total_tris = mesh->index_count / 3u;
    static g3d_shadow_tri_t *s_shtris = NULL;
    static uint32_t s_shtris_cap = 0u;
    if (total_tris > s_shtris_cap) {
        size_t bytes = 0u;
        if (ipo_size_mul_ok((size_t)total_tris, sizeof(g3d_shadow_tri_t), &bytes)) {
            g3d_shadow_tri_t *nt = (g3d_shadow_tri_t *)realloc(s_shtris, bytes);
            if (nt) {
                s_shtris = nt;
                s_shtris_cap = total_tris;
            }
        }
    }
    g3d_shadow_tri_t fallback_shtri;
    g3d_shadow_tri_t *shtri_buf = (s_shtris && s_shtris_cap > 0u) ? s_shtris : &fallback_shtri;
    uint32_t shtri_cap = (s_shtris && s_shtris_cap > 0u) ? s_shtris_cap : 1u;

    ipo_mat4_t vp = ipo_mat4_mul(&ctx->proj, &ctx->view);

    for (int tap = 0; tap < 3; tap++) {
        ipo_vec3_t ld = ipo_vec3_norm(ipo_vec3(light_dir.x + k_shd_ox[tap], light_dir.y, light_dir.z + k_shd_oz[tap]));
        ipo_mat4_t proj_m = ipo_mat4_identity();
        proj_m.m[4]  = -ld.x / ld.y;
        proj_m.m[5]  = 0.0;
        proj_m.m[6]  = -ld.z / ld.y;
        proj_m.m[13] = plane_y + k_shd_yo[tap];

        ipo_mat4_t sh_world = ipo_mat4_mul(&proj_m, model);
        ipo_mat4_t mvp = ipo_mat4_mul(&vp, &sh_world);
        uint8_t tap_id = (uint8_t)(tap + 1);

        if (use_cache) {
            g3d_shv_job_t shv_job = { mesh, &mvp, s_shv, wf, hf };
            if (mesh->vert_count >= 512u && mesh->vert_count <= 2000000u && s_g3d_pool.worker_count > 0) {
                g3d_pool_dispatch(g3d_shv_verts_worker, &shv_job);
            } else {
                g3d_shv_verts_worker(0, 1, &shv_job);
            }
        }

        g3d_shadow_job_t sh_job;
        sh_job.ctx = ctx;
        sh_job.w = w;
        sh_job.h = h;
        sh_job.tris = shtri_buf;
        sh_job.tri_count = 0u;
        sh_job.shd_stamp = s_shd_stamp;
        sh_job.tap_id = tap_id;
        sh_job.shade = shade;

        uint32_t vis_count = 0u;
        uint64_t bbox_px = 0u;

        for (uint32_t ti = 0; ti + 2u < mesh->index_count; ti += 3u) {
            uint32_t i0 = mesh->indices[ti];
            uint32_t i1 = mesh->indices[ti + 1u];
            uint32_t i2 = mesh->indices[ti + 2u];
            g3d_sh_v_t tv0, tv1, tv2;
            const g3d_sh_v_t *p0, *p1, *p2;
            if (use_cache) {
                p0 = &s_shv[i0]; p1 = &s_shv[i1]; p2 = &s_shv[i2];
                if (!p0->valid || !p1->valid || !p2->valid) continue;
            } else {
                const ipo_gfx3d_vertex_t *vs[3] = { &mesh->verts[i0], &mesh->verts[i1], &mesh->verts[i2] };
                g3d_sh_v_t *tvs[3] = { &tv0, &tv1, &tv2 };
                bool ok = true;
                for (int k = 0; k < 3; k++) {
                    const ipo_gfx3d_vertex_t *v = vs[k];
                    ipo_vec4_t c = mat4_mul_vec4(&mvp, (ipo_vec4_t){v->pos.x, v->pos.y, v->pos.z, 1.0});
                    if (c.w <= 0.05) { ok = false; break; }
                    float iw = (float)(1.0 / c.w);
                    tvs[k]->sx = ((float)c.x * iw * 0.5f + 0.5f) * wf;
                    tvs[k]->sy = (0.5f - (float)c.y * iw * 0.5f) * hf;
                    tvs[k]->sz = ((float)c.z * iw) * 0.5f + 0.5f;
                }
                if (!ok) continue;
                p0 = &tv0; p1 = &tv1; p2 = &tv2;
            }

            float sx0 = p0->sx, sy0 = p0->sy, sz0 = p0->sz;
            float sx1 = p1->sx, sy1 = p1->sy, sz1 = p1->sz;
            float sx2 = p2->sx, sy2 = p2->sy, sz2 = p2->sz;
            float area = (sx1 - sx0) * (sy2 - sy0) - (sy1 - sy0) * (sx2 - sx0);
            if (fabsf(area) < 1e-6f) continue;
            float inv_a = 1.0f / area;

            float fmin_x = sx0 < sx1 ? (sx0 < sx2 ? sx0 : sx2) : (sx1 < sx2 ? sx1 : sx2);
            float fmax_x = sx0 > sx1 ? (sx0 > sx2 ? sx0 : sx2) : (sx1 > sx2 ? sx1 : sx2);
            float fmin_y = sy0 < sy1 ? (sy0 < sy2 ? sy0 : sy2) : (sy1 < sy2 ? sy1 : sy2);
            float fmax_y = sy0 > sy1 ? (sy0 > sy2 ? sy0 : sy2) : (sy1 > sy2 ? sy1 : sy2);
            int min_x = g3d_clamp_floor(fmin_x, 0, w - 1);
            int max_x = g3d_clamp_ceil(fmax_x, 0, w - 1);
            int min_y = g3d_clamp_floor(fmin_y, 0, h - 1);
            int max_y = g3d_clamp_ceil(fmax_y, 0, h - 1);
            if (min_x > max_x || min_y > max_y) continue;

            float db0_dx = (sy1 - sy2) * inv_a, db1_dx = (sy2 - sy0) * inv_a;
            float db0_dy = (sx2 - sx1) * inv_a, db1_dy = (sx0 - sx2) * inv_a;
            float fx0 = (float)min_x + 0.5f, fy0 = (float)min_y + 0.5f;
            float b0_row = ((sx1 - fx0) * (sy2 - fy0) - (sy1 - fy0) * (sx2 - fx0)) * inv_a;
            float b1_row = ((sx2 - fx0) * (sy0 - fy0) - (sy2 - fy0) * (sx0 - fx0)) * inv_a;

            float sx_span = (float)(max_x - min_x), sy_span = (float)(max_y - min_y);
            float b0_c1 = b0_row + db0_dx * sx_span, b0_c2 = b0_row + db0_dy * sy_span, b0_c3 = b0_c1 + db0_dy * sy_span;
            float b1_c1 = b1_row + db1_dx * sx_span, b1_c2 = b1_row + db1_dy * sy_span, b1_c3 = b1_c1 + db1_dy * sy_span;
            bool sh_fast = fabsf(b0_row) < 900.0f && fabsf(b0_c1) < 900.0f && fabsf(b0_c2) < 900.0f && fabsf(b0_c3) < 900.0f &&
                           fabsf(b1_row) < 900.0f && fabsf(b1_c1) < 900.0f && fabsf(b1_c2) < 900.0f && fabsf(b1_c3) < 900.0f &&
                           sz0 >= -10.0f && sz0 <= 10.0f && sz1 >= -10.0f && sz1 <= 10.0f && sz2 >= -10.0f && sz2 <= 10.0f;

            g3d_shadow_tri_t *dst = &shtri_buf[vis_count++];
            dst->min_x = min_x; dst->max_x = max_x;
            dst->min_y = min_y; dst->max_y = max_y;
            bbox_px += (uint64_t)(max_x - min_x + 1) * (uint64_t)(max_y - min_y + 1);

            if (sh_fast) {
                dst->sh_fast = 1u;
                dst->q.b0_rq = (int32_t)(b0_row * 1048576.0f); dst->q.b1_rq = (int32_t)(b1_row * 1048576.0f);
                dst->q.db0_xq = (int32_t)(db0_dx * 1048576.0f); dst->q.db1_xq = (int32_t)(db1_dx * 1048576.0f);
                dst->q.db0_yq = (int32_t)(db0_dy * 1048576.0f); dst->q.db1_yq = (int32_t)(db1_dy * 1048576.0f);
                dst->q.sz0_q = (int32_t)(sz0 * 16777216.0f + 0.5f);
                dst->q.sz1_q = (int32_t)(sz1 * 16777216.0f + 0.5f);
                dst->q.sz2_q = (int32_t)(sz2 * 16777216.0f + 0.5f);
            } else {
                dst->sh_fast = 0u;
                dst->f.sz0 = sz0; dst->f.sz1 = sz1; dst->f.sz2 = sz2;
                dst->f.b0_row = b0_row; dst->f.b1_row = b1_row;
                dst->f.db0_dx = db0_dx; dst->f.db1_dx = db1_dx;
                dst->f.db0_dy = db0_dy; dst->f.db1_dy = db1_dy;
            }

            if (vis_count >= shtri_cap) {
                sh_job.tri_count = vis_count;
                if (bbox_px >= 4096u && s_g3d_pool.worker_count > 0) {
                    g3d_pool_dispatch(g3d_shadow_band_worker, &sh_job);
                } else {
                    g3d_shadow_band_worker(0, 1, &sh_job);
                }
                vis_count = 0u;
                bbox_px = 0u;
            }
        }

        if (vis_count > 0u) {
            sh_job.tri_count = vis_count;
            if (bbox_px >= 4096u && s_g3d_pool.worker_count > 0) {
                g3d_pool_dispatch(g3d_shadow_band_worker, &sh_job);
            } else {
                g3d_shadow_band_worker(0, 1, &sh_job);
            }
        }
    }
}

static inline float g3d_aces_gamma(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x < 8.0f) {
        int idx = (int)(x * 128.0f);
        if (idx >= 1024) return s_g3d_aces_gamma_lut[1024];
        return s_g3d_aces_gamma_lut[idx];
    }
    float t = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return sqrtf(t);
}

static inline float g3d_inv_aces_gamma(float g) {
    if (g <= 0.0f) return 0.0f;
    if (g >= 0.992f) return s_g3d_inv_aces_lut[508];
    int idx = (int)(g * 512.0f + 0.5f);
    if (idx > 512) idx = 512;
    return s_g3d_inv_aces_lut[idx];
}

typedef struct {
    ipo_gfx3d_ctx_t *ctx;
    int              w, h;
    float           *aa_rgb;
    float           *aa_lum;
    const float     *bl_a;
    int              bw, bh;
    float            bloom_strength;
} g3d_pfx_job_t;

static void g3d_pfx_bloom_add_worker(int tid, int nthreads, void *opaque) {
    const g3d_pfx_job_t *j = (const g3d_pfx_job_t *)opaque;
    int y0 = 0, y1 = 0;
    g3d_band_range(tid, nthreads, j->h, &y0, &y1);
    int w = j->w, bw = j->bw, bh = j->bh;
    float bs = j->bloom_strength;
    const float *bl = j->bl_a;
    ipo_rgba64f_t *fb = j->ctx->color_fb;
    for (int y = y0; y < y1; y++) {
        float fy = ((float)y + 0.5f) * 0.25f - 0.5f;
        int iy0 = (int)fy; if (fy < (float)iy0) iy0--;
        float ty = fy - (float)iy0;
        int iy1 = iy0 + 1;
        if (iy0 < 0) iy0 = 0; else if (iy0 >= bh) iy0 = bh - 1;
        if (iy1 < 0) iy1 = 0; else if (iy1 >= bh) iy1 = bh - 1;
        size_t r0 = (size_t)iy0 * (size_t)bw;
        size_t r1 = (size_t)iy1 * (size_t)bw;
        ipo_rgba64f_t *crow = fb + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            float fx = ((float)x + 0.5f) * 0.25f - 0.5f;
            int x0 = (int)fx; if (fx < (float)x0) x0--;
            float tx = fx - (float)x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0; else if (x0 >= bw) x0 = bw - 1;
            if (x1 < 0) x1 = 0; else if (x1 >= bw) x1 = bw - 1;
            float w00 = (1.0f - tx) * (1.0f - ty), w10 = tx * (1.0f - ty);
            float w01 = (1.0f - tx) * ty,          w11 = tx * ty;
            size_t o00 = (r0 + (size_t)x0) * 3u, o10 = (r0 + (size_t)x1) * 3u;
            size_t o01 = (r1 + (size_t)x0) * 3u, o11 = (r1 + (size_t)x1) * 3u;
            float br = bl[o00] * w00 + bl[o10] * w10 + bl[o01] * w01 + bl[o11] * w11;
            float bg = bl[o00+1u]*w00 + bl[o10+1u]*w10 + bl[o01+1u]*w01 + bl[o11+1u]*w11;
            float bb = bl[o00+2u]*w00 + bl[o10+2u]*w10 + bl[o01+2u]*w01 + bl[o11+2u]*w11;
            crow[x].r += (double)(br * bs);
            crow[x].g += (double)(bg * bs);
            crow[x].b += (double)(bb * bs);
        }
    }
}

static void g3d_pfx_lum_worker(int tid, int nthreads, void *opaque) {
    const g3d_pfx_job_t *j = (const g3d_pfx_job_t *)opaque;
    int y0 = 0, y1 = 0;
    g3d_band_range(tid, nthreads, j->h, &y0, &y1);
    int w = j->w;
    const ipo_rgba64f_t *fb = j->ctx->color_fb;
    float *aa_rgb = j->aa_rgb;
    float *aa_lum = j->aa_lum;
    for (int y = y0; y < y1; y++) {
        size_t base = (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            size_t i = base + (size_t)x;
            float r = (float)fb[i].r;
            float g = (float)fb[i].g;
            float b = (float)fb[i].b;
            aa_rgb[i * 3u + 0u] = r;
            aa_rgb[i * 3u + 1u] = g;
            aa_rgb[i * 3u + 2u] = b;
            aa_lum[i] = g3d_aces_gamma(0.2126f * r + 0.7152f * g + 0.0722f * b);
        }
    }
}

static void g3d_pfx_fxaa_vig_worker(int tid, int nthreads, void *opaque) {
    const g3d_pfx_job_t *j = (const g3d_pfx_job_t *)opaque;
    int w = j->w, h = j->h;
    int y0 = 0, y1 = 0;
    g3d_band_range(tid, nthreads, h, &y0, &y1);
    const float *aa_rgb = j->aa_rgb;
    const float *aa_lum = j->aa_lum;
    const float *zb = j->ctx->z_buffer;
    ipo_rgba64f_t *fb = j->ctx->color_fb;

    if (aa_rgb && aa_lum) {
        int fy0 = (y0 < 1) ? 1 : y0;
        int fy1 = (y1 > h - 1) ? (h - 1) : y1;
        for (int y = fy0; y < fy1; y++) {
            size_t rM = (size_t)y * (size_t)w;
            size_t rN = rM - (size_t)w;
            size_t rS = rM + (size_t)w;
            for (int x = 1; x < w - 1; x++) {
                size_t iM = rM + (size_t)x;
                size_t iN = rN + (size_t)x, iS = rS + (size_t)x;
                size_t iW = iM - 1u,        iE = iM + 1u;
                float lM = aa_lum[iM];
                float lN = aa_lum[iN], lS = aa_lum[iS];
                float lW = aa_lum[iW], lE = aa_lum[iE];
                float lMax = lM;
                if (lN > lMax) lMax = lN; if (lS > lMax) lMax = lS;
                if (lW > lMax) lMax = lW; if (lE > lMax) lMax = lE;
                float lMin = lM;
                if (lN < lMin) lMin = lN; if (lS < lMin) lMin = lS;
                if (lW < lMin) lMin = lW; if (lE < lMin) lMin = lE;
                float lRange = lMax - lMin;

                float dzMax = 0.0f, zM = 0.0f;
                if (zb) {
                    zM = zb[iM];
                    float d1 = fabsf(zM - zb[iN]), d2 = fabsf(zM - zb[iS]);
                    float d3 = fabsf(zM - zb[iW]), d4 = fabsf(zM - zb[iE]);
                    dzMax = d1;
                    if (d2 > dzMax) dzMax = d2;
                    if (d3 > dzMax) dzMax = d3;
                    if (d4 > dzMax) dzMax = d4;
                }
                float thr = lMax * 0.095f;
                if (thr < 0.036f) thr = 0.036f;
                if (lRange < thr && dzMax <= 0.0022f * (1.0f + zM * 8.0f)) continue;

                float lNW = aa_lum[rN + (size_t)(x - 1)], lNE = aa_lum[rN + (size_t)(x + 1)];
                float lSW = aa_lum[rS + (size_t)(x - 1)], lSE = aa_lum[rS + (size_t)(x + 1)];
                float lCross = 0.25f * (lN + lS + lW + lE);
                float subpix = (lRange > 1e-4f) ? (fabsf(lCross - lM) / lRange) : 0.35f;
                if (subpix > 1.0f) subpix = 1.0f;
                subpix = subpix * subpix * (3.0f - 2.0f * subpix) * 0.52f;

                float edgeH = fabsf(-2.0f * lW + lNW + lSW) + 2.0f * fabsf(-2.0f * lM + lN + lS) + fabsf(-2.0f * lE + lNE + lSE);
                float edgeV = fabsf(-2.0f * lN + lNW + lNE) + 2.0f * fabsf(-2.0f * lM + lW + lE) + fabsf(-2.0f * lS + lSW + lSE);
                bool horz = (edgeH >= edgeV);

                float lNeg = horz ? lN : lW;
                float lPos = horz ? lS : lE;
                float gradNeg = fabsf(lNeg - lM);
                float gradPos = fabsf(lPos - lM);
                if (zb) {
                    gradNeg += fabsf(zM - zb[horz ? iN : iW]) * 2.5f;
                    gradPos += fabsf(zM - zb[horz ? iS : iE]) * 2.5f;
                }
                bool useNeg = (gradNeg >= gradPos);
                size_t iOther = horz ? (useNeg ? iN : iS) : (useNeg ? iW : iE);
                float lOther = useNeg ? lNeg : lPos;
                float lAvg = 0.5f * (lM + lOther);
                float gradTol = 0.25f * ((gradNeg > gradPos) ? gradNeg : gradPos);
                if (gradTol < 0.015f) gradTol = 0.015f;

                int dx_tan = horz ? 1 : 0, dy_tan = horz ? 0 : 1;
                int perp_d = useNeg ? -1 : 1;
                int dn = 6, dp = 6;
                float endN = lM, endP = lM;
                for (int s = 1; s <= 6; s++) {
                    int sx = x - dx_tan * s, sy = y - dy_tan * s;
                    if (sx < 0 || sy < 0) { dn = s; break; }
                    int px2 = horz ? sx : (sx + perp_d < 0 || sx + perp_d >= w ? sx : sx + perp_d);
                    int py2 = horz ? (sy + perp_d < 0 || sy + perp_d >= h ? sy : sy + perp_d) : sy;
                    float ln = 0.5f * (aa_lum[(size_t)sy * (size_t)w + (size_t)sx] +
                                       aa_lum[(size_t)py2 * (size_t)w + (size_t)px2]);
                    endN = ln;
                    if (fabsf(ln - lAvg) >= gradTol) { dn = s; break; }
                }
                for (int s = 1; s <= 6; s++) {
                    int sx = x + dx_tan * s, sy = y + dy_tan * s;
                    if (sx >= w || sy >= h) { dp = s; break; }
                    int px2 = horz ? sx : (sx + perp_d < 0 || sx + perp_d >= w ? sx : sx + perp_d);
                    int py2 = horz ? (sy + perp_d < 0 || sy + perp_d >= h ? sy : sy + perp_d) : sy;
                    float lp = 0.5f * (aa_lum[(size_t)sy * (size_t)w + (size_t)sx] +
                                       aa_lum[(size_t)py2 * (size_t)w + (size_t)px2]);
                    endP = lp;
                    if (fabsf(lp - lAvg) >= gradTol) { dp = s; break; }
                }

                float edgeBlend = 0.0f;
                bool isNegNear = (dn < dp);
                int dNearest = isNegNear ? dn : dp;
                float endNear = isNegNear ? endN : endP;
                if (((endNear - lAvg) < 0.0f) != ((lM - lAvg) < 0.0f)) {
                    edgeBlend = 0.5f - (float)dNearest / (float)(dn + dp);
                    if (edgeBlend < 0.0f) edgeBlend = 0.0f;
                }
                float blend = (subpix > edgeBlend) ? subpix : edgeBlend;
                if (blend > 0.72f) blend = 0.72f;
                if (blend > 0.005f) {
                    float omb = 1.0f - blend;
                    float mr = g3d_aces_gamma(aa_rgb[iM * 3u + 0u]) * omb + g3d_aces_gamma(aa_rgb[iOther * 3u + 0u]) * blend;
                    float mg = g3d_aces_gamma(aa_rgb[iM * 3u + 1u]) * omb + g3d_aces_gamma(aa_rgb[iOther * 3u + 1u]) * blend;
                    float mb = g3d_aces_gamma(aa_rgb[iM * 3u + 2u]) * omb + g3d_aces_gamma(aa_rgb[iOther * 3u + 2u]) * blend;
                    fb[iM].r = (double)g3d_inv_aces_gamma(mr);
                    fb[iM].g = (double)g3d_inv_aces_gamma(mg);
                    fb[iM].b = (double)g3d_inv_aces_gamma(mb);
                }
            }
        }
    }

    float inv_w = 2.0f / (float)w;
    float inv_h = 2.0f / (float)h;
    for (int y = y0; y < y1; y++) {
        float ny = ((float)y + 0.5f) * inv_h - 1.0f;
        float ny2 = ny * ny;
        ipo_rgba64f_t *row = fb + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            float nx = ((float)x + 0.5f) * inv_w - 1.0f;
            float r2 = nx * nx + ny2;
            float vig = 1.0f - 0.14f * r2;
            if (vig < 0.72f) vig = 0.72f;
            row[x].r *= (double)vig;
            row[x].g *= (double)vig;
            row[x].b *= (double)vig;
        }
    }
}

void ipo_gfx3d_apply_postfx(ipo_gfx3d_ctx_t *ctx, double bloom_strength, bool enable_aa) {
    if (!ctx || !ctx->color_fb || ctx->width < 4 || ctx->height < 4) return;
    extern bool g3d_gpu_is_frame_active(const ipo_gfx3d_ctx_t *ctx);
    if (g3d_gpu_is_frame_active(ctx)) {
        g3d_set_postfx_done(ctx, true);
        return;
    }
    g3d_init_luts();
    g3d_set_postfx_done(ctx, true);

    int w = ctx->width, h = ctx->height;

    if (bloom_strength > 0.001) {
        int bw = (w + 3) / 4;
        int bh = (h + 3) / 4;
        size_t bfloats = 0u;
        if (ipo_size_mul_ok((size_t)bw * (size_t)bh, 3u, &bfloats)) {
            static float *s_bl_a = NULL;
            static float *s_bl_b = NULL;
            static size_t s_bl_cap = 0u;
            if (bfloats > s_bl_cap) {
                size_t bytes = 0u;
                if (ipo_size_mul_ok(bfloats, sizeof(float), &bytes)) {
                    float *na = (float *)realloc(s_bl_a, bytes);
                    float *nb = (float *)realloc(s_bl_b, bytes);
                    if (na) s_bl_a = na;
                    if (nb) s_bl_b = nb;
                    if (na && nb) s_bl_cap = bfloats;
                }
            }
            if (s_bl_a && s_bl_b && s_bl_cap >= bfloats) {
                for (int by = 0; by < bh; by++) {
                    int y0 = by * 4;
                    int y1 = (y0 + 4 < h) ? (y0 + 4) : h;
                    for (int bx = 0; bx < bw; bx++) {
                        int x0 = bx * 4;
                        int x1 = (x0 + 4 < w) ? (x0 + 4) : w;
                        float sr = 0.0f, sg = 0.0f, sb = 0.0f;
                        int cnt = 0;
                        for (int py = y0; py < y1; py++) {
                            const ipo_rgba64f_t *row = ctx->color_fb + (size_t)py * (size_t)w;
                            for (int px = x0; px < x1; px++) {
                                sr += (float)row[px].r;
                                sg += (float)row[px].g;
                                sb += (float)row[px].b;
                                cnt++;
                            }
                        }
                        float inv_c = (cnt > 0) ? (1.0f / (float)cnt) : 0.0f;
                        sr *= inv_c; sg *= inv_c; sb *= inv_c;
                        float lum = 0.2126f * sr + 0.7152f * sg + 0.0722f * sb;
                        float thresh = 1.65f, knee = 0.65f;
                        float soft = lum - thresh + knee;
                        if (soft < 0.0f) soft = 0.0f;
                        if (soft > 2.0f * knee) soft = 2.0f * knee;
                        soft = (soft * soft) / (4.0f * knee + 1e-5f);
                        float contrib = (lum > thresh) ? (lum - thresh) : 0.0f;
                        if (soft > contrib) contrib = soft;
                        float weight = (lum > 1e-5f) ? (contrib / lum) : 0.0f;
                        if (weight > 1.5f) weight = 1.5f;
                        size_t o = ((size_t)by * (size_t)bw + (size_t)bx) * 3u;
                        s_bl_a[o + 0u] = sr * weight;
                        s_bl_a[o + 1u] = sg * weight;
                        s_bl_a[o + 2u] = sb * weight;
                    }
                }
                static const float k_g7[7] = { 0.030f, 0.105f, 0.222f, 0.286f, 0.222f, 0.105f, 0.030f };
                for (int by = 0; by < bh; by++) {
                    size_t row = (size_t)by * (size_t)bw;
                    for (int bx = 0; bx < bw; bx++) {
                        float ar = 0.0f, ag = 0.0f, ab = 0.0f;
                        for (int k = -3; k <= 3; k++) {
                            int sx = bx + k;
                            if (sx < 0) sx = 0; else if (sx >= bw) sx = bw - 1;
                            size_t o = (row + (size_t)sx) * 3u;
                            float wt = k_g7[k + 3];
                            ar += s_bl_a[o + 0u] * wt;
                            ag += s_bl_a[o + 1u] * wt;
                            ab += s_bl_a[o + 2u] * wt;
                        }
                        size_t dst = (row + (size_t)bx) * 3u;
                        s_bl_b[dst + 0u] = ar;
                        s_bl_b[dst + 1u] = ag;
                        s_bl_b[dst + 2u] = ab;
                    }
                }
                for (int by = 0; by < bh; by++) {
                    for (int bx = 0; bx < bw; bx++) {
                        float ar = 0.0f, ag = 0.0f, ab = 0.0f;
                        for (int k = -3; k <= 3; k++) {
                            int sy = by + k;
                            if (sy < 0) sy = 0; else if (sy >= bh) sy = bh - 1;
                            size_t o = ((size_t)sy * (size_t)bw + (size_t)bx) * 3u;
                            float wt = k_g7[k + 3];
                            ar += s_bl_b[o + 0u] * wt;
                            ag += s_bl_b[o + 1u] * wt;
                            ab += s_bl_b[o + 2u] * wt;
                        }
                        size_t dst = ((size_t)by * (size_t)bw + (size_t)bx) * 3u;
                        s_bl_a[dst + 0u] = ar;
                        s_bl_a[dst + 1u] = ag;
                        s_bl_a[dst + 2u] = ab;
                    }
                }
                g3d_pfx_job_t bjob = {
                    .ctx = ctx, .w = w, .h = h, .aa_rgb = NULL, .aa_lum = NULL,
                    .bl_a = s_bl_a, .bw = bw, .bh = bh, .bloom_strength = (float)bloom_strength
                };
                if (h >= 32) g3d_pool_dispatch(g3d_pfx_bloom_add_worker, &bjob);
                else g3d_pfx_bloom_add_worker(0, 1, &bjob);
            }
        }
    }

    float *aa_rgb_use = NULL;
    float *aa_lum_use = NULL;
    if (enable_aa && w >= 6 && h >= 6) {
        size_t px_n = (size_t)w * (size_t)h;
        size_t rgb_n = 0u;
        if (ipo_size_mul_ok(px_n, 3u, &rgb_n)) {
            static float *s_aa_rgb = NULL;
            static float *s_aa_lum = NULL;
            static size_t s_aa_cap = 0u;
            if (px_n > s_aa_cap) {
                size_t b_rgb = 0u, b_lum = 0u;
                if (ipo_size_mul_ok(rgb_n, sizeof(float), &b_rgb) &&
                    ipo_size_mul_ok(px_n, sizeof(float), &b_lum)) {
                    float *nr = (float *)realloc(s_aa_rgb, b_rgb);
                    float *nl = (float *)realloc(s_aa_lum, b_lum);
                    if (nr) s_aa_rgb = nr;
                    if (nl) s_aa_lum = nl;
                    if (nr && nl) s_aa_cap = px_n;
                }
            }
            if (s_aa_rgb && s_aa_lum && s_aa_cap >= px_n) {
                aa_rgb_use = s_aa_rgb;
                aa_lum_use = s_aa_lum;
                g3d_pfx_job_t ljob = {
                    .ctx = ctx, .w = w, .h = h, .aa_rgb = aa_rgb_use, .aa_lum = aa_lum_use,
                    .bl_a = NULL, .bw = 0, .bh = 0, .bloom_strength = 0.0f
                };
                if (h >= 32) g3d_pool_dispatch(g3d_pfx_lum_worker, &ljob);
                else g3d_pfx_lum_worker(0, 1, &ljob);
            }
        }
    }

    g3d_pfx_job_t fjob = {
        .ctx = ctx, .w = w, .h = h, .aa_rgb = aa_rgb_use, .aa_lum = aa_lum_use,
        .bl_a = NULL, .bw = 0, .bh = 0, .bloom_strength = 0.0f
    };
    if (h >= 32) g3d_pool_dispatch(g3d_pfx_fxaa_vig_worker, &fjob);
    else g3d_pfx_fxaa_vig_worker(0, 1, &fjob);
    __atomic_store_n(&s_g3d_pool.hot, 0, __ATOMIC_RELEASE);
}

bool ipo_gfx3d_mesh_cube(ipo_gfx3d_mesh_t *out, double s) {
    if (!out) return false;
    static const double fn[6][3] = {
        {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}
    };
    static const double fv[6][4][3] = {
        {{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}},
        {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}},
        {{1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1}},
        {{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1}},
        {{-1,1,1},{1,1,1},{1,1,-1},{-1,1,-1}},
        {{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}}
    };
    static const double fuv[4][2] = {{0,1},{1,1},{1,0},{0,0}};
    out->verts = (ipo_gfx3d_vertex_t *)malloc(24u * sizeof(ipo_gfx3d_vertex_t));
    out->indices = (uint32_t *)malloc(36u * sizeof(uint32_t));
    if (!out->verts || !out->indices) { ipo_gfx3d_mesh_free(out); return false; }
    out->vert_count = 24u;
    out->index_count = 36u;
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) {
            ipo_gfx3d_vertex_t *v = &out->verts[f * 4 + k];
            v->pos = ipo_vec3(fv[f][k][0] * s, fv[f][k][1] * s, fv[f][k][2] * s);
            v->normal = ipo_vec3(fn[f][0], fn[f][1], fn[f][2]);
            v->u = fuv[k][0]; v->v = fuv[k][1];
            v->color = (ipo_rgba64f_t){ 1.0, 1.0, 1.0, 1.0 };
        }
        uint32_t b = (uint32_t)(f * 4);
        out->indices[f*6 + 0] = b; out->indices[f*6 + 1] = b+1; out->indices[f*6 + 2] = b+2;
        out->indices[f*6 + 3] = b; out->indices[f*6 + 4] = b+2; out->indices[f*6 + 5] = b+3;
    }
    return true;
}

bool ipo_gfx3d_mesh_rounded_cube(ipo_gfx3d_mesh_t *out, double half_extent, double corner_radius, int subdiv) {
    if (!out || half_extent <= 1e-6) return false;
    memset(out, 0, sizeof(*out));
    if (subdiv < 4) subdiv = 4;

    double s = half_extent;
    double r = corner_radius;
    if (r < 0.002 * s) r = 0.002 * s;
    if (r > 0.45 * s)  r = 0.45 * s;
    double inner = s - r;

    int bevel_segs = (subdiv >= 10) ? (subdiv / 3) : ((subdiv >= 6) ? 2 : 1);
    int flat_segs = subdiv - 2 * bevel_segs;
    if (flat_segs < 1) {
        bevel_segs = (subdiv - 1) / 2;
        if (bevel_segs < 1) bevel_segs = 1;
        flat_segs = subdiv - 2 * bevel_segs;
    }

    int grid = subdiv + 1;
    double *ts = (double *)malloc((size_t)grid * sizeof(double));
    if (!ts) return false;
    for (int k = 0; k <= subdiv; k++) {
        if (k <= bevel_segs) {
            ts[k] = -s + r * ((double)k / (double)bevel_segs);
        } else if (k <= bevel_segs + flat_segs) {
            ts[k] = -inner + (2.0 * inner) * ((double)(k - bevel_segs) / (double)flat_segs);
        } else {
            ts[k] = inner + r * ((double)(k - bevel_segs - flat_segs) / (double)bevel_segs);
        }
    }

    uint32_t face_verts = (uint32_t)(grid * grid);
    uint32_t face_indices = (uint32_t)(subdiv * subdiv * 6);
    uint32_t vc = face_verts * 6u;
    uint32_t ic = face_indices * 6u;
    out->verts = (ipo_gfx3d_vertex_t *)malloc((size_t)vc * sizeof(ipo_gfx3d_vertex_t));
    out->indices = (uint32_t *)malloc((size_t)ic * sizeof(uint32_t));
    if (!out->verts || !out->indices) {
        free(ts);
        ipo_gfx3d_mesh_free(out);
        return false;
    }
    out->vert_count = vc;
    out->index_count = ic;

    static const double fn[6][3] = {
        {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}
    };
    static const double fu[6][3] = {
        {1,0,0}, {-1,0,0}, {0,0,-1}, {0,0,1}, {1,0,0}, {1,0,0}
    };
    static const double fv[6][3] = {
        {0,1,0}, {0,1,0}, {0,1,0}, {0,1,0}, {0,0,-1}, {0,0,1}
    };

    uint32_t vi = 0u, ii = 0u;
    for (int f = 0; f < 6; f++) {
        uint32_t base = vi;
        for (int j = 0; j < grid; j++) {
            double pv = ts[j];
            for (int i = 0; i < grid; i++) {
                double pu = ts[i];
                double bx = fn[f][0] * s + fu[f][0] * pu + fv[f][0] * pv;
                double by = fn[f][1] * s + fu[f][1] * pu + fv[f][1] * pv;
                double bz = fn[f][2] * s + fu[f][2] * pu + fv[f][2] * pv;

                double cx = (bx < -inner) ? -inner : ((bx > inner) ? inner : bx);
                double cy = (by < -inner) ? -inner : ((by > inner) ? inner : by);
                double cz = (bz < -inner) ? -inner : ((bz > inner) ? inner : bz);

                double dx = bx - cx, dy = by - cy, dz = bz - cz;
                double dlen = sqrt(dx * dx + dy * dy + dz * dz);
                ipo_vec3_t nrm, pos;
                if (dlen > 1e-12) {
                    double inv_d = 1.0 / dlen;
                    nrm = ipo_vec3(dx * inv_d, dy * inv_d, dz * inv_d);
                    pos = ipo_vec3(cx + nrm.x * r, cy + nrm.y * r, cz + nrm.z * r);
                } else {
                    nrm = ipo_vec3(fn[f][0], fn[f][1], fn[f][2]);
                    pos = ipo_vec3(bx, by, bz);
                }

                ipo_gfx3d_vertex_t *v = &out->verts[vi++];
                v->pos = pos;
                v->normal = nrm;
                v->u = 0.5 * (pu / s + 1.0);
                v->v = 1.0 - 0.5 * (pv / s + 1.0);
                v->color = (ipo_rgba64f_t){ 1.0, 1.0, 1.0, 1.0 };
            }
        }
        for (int j = 0; j < subdiv; j++) {
            for (int i = 0; i < subdiv; i++) {
                uint32_t v00 = base + (uint32_t)(j * grid + i);
                uint32_t v10 = v00 + 1u;
                uint32_t v01 = v00 + (uint32_t)grid;
                uint32_t v11 = v01 + 1u;
                out->indices[ii++] = v00; out->indices[ii++] = v10; out->indices[ii++] = v11;
                out->indices[ii++] = v00; out->indices[ii++] = v11; out->indices[ii++] = v01;
            }
        }
    }
    free(ts);
    return true;
}

typedef struct {
    ipo_gfx3d_vertex_t *v;
    uint32_t            vc, vcap;
    uint32_t           *idx;
    uint32_t            ic, icap;
    bool                oom;
} g3d_mb_t;

static uint32_t g3d_mb_pushv(g3d_mb_t *mb, ipo_vec3_t pos, ipo_vec3_t nrm, double u, double v) {
    if (mb->oom) return 0u;
    if (mb->vc >= mb->vcap) {
        uint32_t nc = mb->vcap ? (mb->vcap * 2u) : 512u;
        size_t bytes = 0u;
        if (nc <= mb->vcap || !ipo_size_mul_ok((size_t)nc, sizeof(ipo_gfx3d_vertex_t), &bytes)) {
            mb->oom = true; return 0u;
        }
        ipo_gfx3d_vertex_t *nv = (ipo_gfx3d_vertex_t *)realloc(mb->v, bytes);
        if (!nv) { mb->oom = true; return 0u; }
        mb->v = nv;
        mb->vcap = nc;
    }
    uint32_t id = mb->vc++;
    mb->v[id].pos = pos;
    mb->v[id].normal = ipo_vec3_norm(nrm);
    mb->v[id].u = u;
    mb->v[id].v = v;
    mb->v[id].color = (ipo_rgba64f_t){ 1.0, 1.0, 1.0, 1.0 };
    return id;
}

static void g3d_mb_quad(g3d_mb_t *mb, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (mb->oom) return;
    if (mb->ic + 6u > mb->icap) {
        uint32_t nc = mb->icap ? (mb->icap * 2u) : 1024u;
        while (nc < mb->ic + 6u) nc *= 2u;
        size_t bytes = 0u;
        if (!ipo_size_mul_ok((size_t)nc, sizeof(uint32_t), &bytes)) {
            mb->oom = true; return;
        }
        uint32_t *ni = (uint32_t *)realloc(mb->idx, bytes);
        if (!ni) { mb->oom = true; return; }
        mb->idx = ni;
        mb->icap = nc;
    }
    mb->idx[mb->ic++] = a; mb->idx[mb->ic++] = b; mb->idx[mb->ic++] = c;
    mb->idx[mb->ic++] = a; mb->idx[mb->ic++] = c; mb->idx[mb->ic++] = d;
}

bool ipo_gfx3d_mesh_gear(ipo_gfx3d_mesh_t *out, double inner_radius, double outer_radius,
                         double width, int teeth, double tooth_depth, double chamfer) {
    if (!out || teeth < 4 || width <= 1e-5 || outer_radius <= inner_radius + 1e-4) return false;
    memset(out, 0, sizeof(*out));

    double r0 = inner_radius;
    double r1 = outer_radius - tooth_depth * 0.5;
    double r2 = outer_radius + tooth_depth * 0.5;
    if (r1 <= r0 + 1e-4) r1 = r0 + 0.25 * (outer_radius - r0);
    double hw = 0.5 * width;
    double ch = chamfer;
    if (ch < 0.0) ch = 0.0;
    if (ch > hw * 0.35) ch = hw * 0.35;
    if (ch > (r1 - r0) * 0.22) ch = (r1 - r0) * 0.22;
    if (ch > tooth_depth * 0.22) ch = tooth_depth * 0.22;

    const int k_sub = 8;
    int n_pts = teeth * k_sub;
    double *px = (double *)malloc((size_t)n_pts * sizeof(double));
    double *py = (double *)malloc((size_t)n_pts * sizeof(double));
    double *pin_x = (double *)malloc((size_t)n_pts * sizeof(double));
    double *pin_y = (double *)malloc((size_t)n_pts * sizeof(double));
    double *nx2 = (double *)malloc((size_t)n_pts * sizeof(double));
    double *ny2 = (double *)malloc((size_t)n_pts * sizeof(double));
    double *bx0 = (double *)malloc((size_t)n_pts * sizeof(double));
    double *by0 = (double *)malloc((size_t)n_pts * sizeof(double));
    double *bx1 = (double *)malloc((size_t)n_pts * sizeof(double));
    double *by1 = (double *)malloc((size_t)n_pts * sizeof(double));
    if (!px || !py || !pin_x || !pin_y || !nx2 || !ny2 || !bx0 || !by0 || !bx1 || !by1) {
        free(px); free(py); free(pin_x); free(pin_y); free(nx2); free(ny2);
        free(bx0); free(by0); free(bx1); free(by1);
        return false;
    }

    static const double k_phase[8]  = { 0.00, 0.11, 0.21, 0.29, 0.39, 0.47, 0.57, 0.78 };
    static const double k_rfrac[8]  = { 0.00, 0.12, 0.72, 1.00, 1.00, 0.72, 0.12, 0.00 };
    double da = (2.0 * M_PI) / (double)teeth;
    double td = r2 - r1;

    for (int it = 0; it < teeth; it++) {
        double base_a = (double)it * da;
        for (int u = 0; u < k_sub; u++) {
            int p = it * k_sub + u;
            double th = base_a + da * k_phase[u];
            double rad = r1 + td * k_rfrac[u];
            double c = cos(th), s = sin(th);
            px[p] = rad * c; py[p] = rad * s;
            bx0[p] = r0 * c; by0[p] = r0 * s;
            bx1[p] = (r0 + ch) * c; by1[p] = (r0 + ch) * s;
        }
    }

    for (int p = 0; p < n_pts; p++) {
        int pm = (p - 1 + n_pts) % n_pts;
        int pp = (p + 1) % n_pts;
        double dx0 = px[p] - px[pm], dy0 = py[p] - py[pm];
        double l0 = sqrt(dx0 * dx0 + dy0 * dy0);
        double dx1 = px[pp] - px[p], dy1 = py[pp] - py[p];
        double l1 = sqrt(dx1 * dx1 + dy1 * dy1);
        double n0x = (l0 > 1e-12) ? (dy0 / l0) : 0.0, n0y = (l0 > 1e-12) ? (-dx0 / l0) : 0.0;
        double n1x = (l1 > 1e-12) ? (dy1 / l1) : 0.0, n1y = (l1 > 1e-12) ? (-dx1 / l1) : 0.0;
        double sx = n0x + n1x, sy = n0y + n1y;
        double sl = sqrt(sx * sx + sy * sy);
        if (sl > 1e-12) { sx /= sl; sy /= sl; }
        else { sx = px[p] / (outer_radius + 1e-6); sy = py[p] / (outer_radius + 1e-6); }
        nx2[p] = sx; ny2[p] = sy;
        pin_x[p] = px[p] - ch * sx;
        pin_y[p] = py[p] - ch * sy;
    }

    g3d_mb_t mb;
    memset(&mb, 0, sizeof(mb));
    double hw_in = hw - ch;

    for (int p = 0; p < n_pts; p++) {
        int pp = (p + 1) % n_pts;
        double rad0 = sqrt(px[p]*px[p] + py[p]*py[p]);
        double rad1 = sqrt(px[pp]*px[pp] + py[pp]*py[pp]);
        double ur0 = (rad0 - r0) / (r2 - r0 + 1e-6);
        double ur1 = (rad1 - r0) / (r2 - r0 + 1e-6);
        double th0 = (double)p / (double)n_pts;
        double th1 = (double)(p + 1) / (double)n_pts;

        /* Front flat face (+Z) */
        uint32_t f_b0 = g3d_mb_pushv(&mb, ipo_vec3(bx1[p],  by1[p],  hw), ipo_vec3(0,0,1), th0, 0.0);
        uint32_t f_o0 = g3d_mb_pushv(&mb, ipo_vec3(pin_x[p],pin_y[p],hw), ipo_vec3(0,0,1), th0, ur0);
        uint32_t f_o1 = g3d_mb_pushv(&mb, ipo_vec3(pin_x[pp],pin_y[pp],hw), ipo_vec3(0,0,1), th1, ur1);
        uint32_t f_b1 = g3d_mb_pushv(&mb, ipo_vec3(bx1[pp], by1[pp], hw), ipo_vec3(0,0,1), th1, 0.0);
        g3d_mb_quad(&mb, f_b0, f_o0, f_o1, f_b1);

        /* Back flat face (-Z) */
        uint32_t b_o0 = g3d_mb_pushv(&mb, ipo_vec3(pin_x[p],pin_y[p],-hw), ipo_vec3(0,0,-1), th0, ur0);
        uint32_t b_b0 = g3d_mb_pushv(&mb, ipo_vec3(bx1[p],  by1[p],  -hw), ipo_vec3(0,0,-1), th0, 0.0);
        uint32_t b_b1 = g3d_mb_pushv(&mb, ipo_vec3(bx1[pp], by1[pp], -hw), ipo_vec3(0,0,-1), th1, 0.0);
        uint32_t b_o1 = g3d_mb_pushv(&mb, ipo_vec3(pin_x[pp],pin_y[pp],-hw), ipo_vec3(0,0,-1), th1, ur1);
        g3d_mb_quad(&mb, b_o0, b_b0, b_b1, b_o1);

        if (ch > 1e-6) {
            /* Front outer chamfer strip */
            ipo_vec3_t fn0 = ipo_vec3(nx2[p], ny2[p], 1.0);
            ipo_vec3_t fn1 = ipo_vec3(nx2[pp], ny2[pp], 1.0);
            uint32_t fc_i0 = g3d_mb_pushv(&mb, ipo_vec3(pin_x[p],  pin_y[p],  hw),    fn0, th0, ur0);
            uint32_t fc_e0 = g3d_mb_pushv(&mb, ipo_vec3(px[p],     py[p],     hw_in), fn0, th0, ur0);
            uint32_t fc_e1 = g3d_mb_pushv(&mb, ipo_vec3(px[pp],    py[pp],    hw_in), fn1, th1, ur1);
            uint32_t fc_i1 = g3d_mb_pushv(&mb, ipo_vec3(pin_x[pp], pin_y[pp], hw),    fn1, th1, ur1);
            g3d_mb_quad(&mb, fc_i0, fc_e0, fc_e1, fc_i1);

            /* Back outer chamfer strip */
            ipo_vec3_t bn0 = ipo_vec3(nx2[p], ny2[p], -1.0);
            ipo_vec3_t bn1 = ipo_vec3(nx2[pp], ny2[pp], -1.0);
            uint32_t bc_e0 = g3d_mb_pushv(&mb, ipo_vec3(px[p],     py[p],     -hw_in), bn0, th0, ur0);
            uint32_t bc_i0 = g3d_mb_pushv(&mb, ipo_vec3(pin_x[p],  pin_y[p],  -hw),    bn0, th0, ur0);
            uint32_t bc_i1 = g3d_mb_pushv(&mb, ipo_vec3(pin_x[pp], pin_y[pp], -hw),    bn1, th1, ur1);
            uint32_t bc_e1 = g3d_mb_pushv(&mb, ipo_vec3(px[pp],    py[pp],    -hw_in), bn1, th1, ur1);
            g3d_mb_quad(&mb, bc_e0, bc_i0, bc_i1, bc_e1);

            /* Front bore chamfer strip */
            ipo_vec3_t fbn0 = ipo_vec3(-bx0[p], -by0[p], r0);
            ipo_vec3_t fbn1 = ipo_vec3(-bx0[pp], -by0[pp], r0);
            uint32_t fbi0 = g3d_mb_pushv(&mb, ipo_vec3(bx0[p], by0[p], hw_in), fbn0, th0, 0.0);
            uint32_t fbe0 = g3d_mb_pushv(&mb, ipo_vec3(bx1[p], by1[p], hw),    fbn0, th0, 0.0);
            uint32_t fbe1 = g3d_mb_pushv(&mb, ipo_vec3(bx1[pp],by1[pp],hw),    fbn1, th1, 0.0);
            uint32_t fbi1 = g3d_mb_pushv(&mb, ipo_vec3(bx0[pp],by0[pp],hw_in), fbn1, th1, 0.0);
            g3d_mb_quad(&mb, fbi0, fbe0, fbe1, fbi1);

            /* Back bore chamfer strip */
            ipo_vec3_t bbn0 = ipo_vec3(-bx0[p], -by0[p], -r0);
            ipo_vec3_t bbn1 = ipo_vec3(-bx0[pp], -by0[pp], -r0);
            uint32_t bbe0 = g3d_mb_pushv(&mb, ipo_vec3(bx1[p], by1[p], -hw),    bbn0, th0, 0.0);
            uint32_t bbi0 = g3d_mb_pushv(&mb, ipo_vec3(bx0[p], by0[p], -hw_in), bbn0, th0, 0.0);
            uint32_t bbi1 = g3d_mb_pushv(&mb, ipo_vec3(bx0[pp],by0[pp],-hw_in), bbn1, th1, 0.0);
            uint32_t bbe1 = g3d_mb_pushv(&mb, ipo_vec3(bx1[pp],by1[pp],-hw),    bbn1, th1, 0.0);
            g3d_mb_quad(&mb, bbe0, bbi0, bbi1, bbe1);
        }

        /* Crisp outer tooth wall / involute flank */
        double sx = px[pp] - px[p], sy = py[pp] - py[p];
        ipo_vec3_t wn = ipo_vec3(sy, -sx, 0.0);
        uint32_t w_t0 = g3d_mb_pushv(&mb, ipo_vec3(px[p],  py[p],  hw_in),  wn, th0, 1.0);
        uint32_t w_b0 = g3d_mb_pushv(&mb, ipo_vec3(px[p],  py[p],  -hw_in), wn, th0, 0.0);
        uint32_t w_b1 = g3d_mb_pushv(&mb, ipo_vec3(px[pp], py[pp], -hw_in), wn, th1, 0.0);
        uint32_t w_t1 = g3d_mb_pushv(&mb, ipo_vec3(px[pp], py[pp], hw_in),  wn, th1, 1.0);
        g3d_mb_quad(&mb, w_t0, w_b0, w_b1, w_t1);

        /* Inner smooth hub bore cylinder */
        ipo_vec3_t in0 = ipo_vec3(-bx0[p], -by0[p], 0.0);
        ipo_vec3_t in1 = ipo_vec3(-bx0[pp], -by0[pp], 0.0);
        uint32_t i_b0 = g3d_mb_pushv(&mb, ipo_vec3(bx0[p],  by0[p],  -hw_in), in0, th0, 0.0);
        uint32_t i_t0 = g3d_mb_pushv(&mb, ipo_vec3(bx0[p],  by0[p],  hw_in),  in0, th0, 1.0);
        uint32_t i_t1 = g3d_mb_pushv(&mb, ipo_vec3(bx0[pp], by0[pp], hw_in),  in1, th1, 1.0);
        uint32_t i_b1 = g3d_mb_pushv(&mb, ipo_vec3(bx0[pp], by0[pp], -hw_in), in1, th1, 0.0);
        g3d_mb_quad(&mb, i_b0, i_t0, i_t1, i_b1);
    }

    free(px); free(py); free(pin_x); free(pin_y); free(nx2); free(ny2);
    free(bx0); free(by0); free(bx1); free(by1);

    if (mb.oom) {
        free(mb.v);
        free(mb.idx);
        return false;
    }
    out->verts = mb.v;
    out->vert_count = mb.vc;
    out->indices = mb.idx;
    out->index_count = mb.ic;
    return true;
}

bool ipo_gfx3d_mesh_sphere(ipo_gfx3d_mesh_t *out, double radius, int stacks, int slices) {
    if (!out || stacks < 4 || slices < 6) return false;
    uint32_t vc = (uint32_t)((stacks + 1) * (slices + 1));
    uint32_t ic = (uint32_t)(stacks * slices * 6);
    out->verts = (ipo_gfx3d_vertex_t *)malloc(vc * sizeof(ipo_gfx3d_vertex_t));
    out->indices = (uint32_t *)malloc(ic * sizeof(uint32_t));
    if (!out->verts || !out->indices) { ipo_gfx3d_mesh_free(out); return false; }
    out->vert_count = vc;
    out->index_count = ic;
    uint32_t vi = 0;
    for (int i = 0; i <= stacks; i++) {
        double v = (double)i / (double)stacks;
        double phi = v * M_PI;
        double sp = sin(phi), cp = cos(phi);
        for (int j = 0; j <= slices; j++) {
            double u = (double)j / (double)slices;
            double theta = u * 2.0 * M_PI;
            double nx = sp * cos(theta), ny = cp, nz = sp * sin(theta);
            out->verts[vi].pos = ipo_vec3(nx * radius, ny * radius, nz * radius);
            out->verts[vi].normal = ipo_vec3(nx, ny, nz);
            out->verts[vi].u = u; out->verts[vi].v = v;
            out->verts[vi].color = (ipo_rgba64f_t){ 1.0, 1.0, 1.0, 1.0 };
            vi++;
        }
    }
    uint32_t ii = 0;
    for (int i = 0; i < stacks; i++) {
        for (int j = 0; j < slices; j++) {
            uint32_t a = (uint32_t)(i * (slices + 1) + j);
            uint32_t b = (uint32_t)((i + 1) * (slices + 1) + j);
            out->indices[ii++] = a; out->indices[ii++] = a + 1u; out->indices[ii++] = b;
            out->indices[ii++] = b; out->indices[ii++] = a + 1u; out->indices[ii++] = b + 1u;
        }
    }
    return true;
}

bool ipo_gfx3d_mesh_torus(ipo_gfx3d_mesh_t *out, double major_r, double minor_r, int segs, int sides) {
    if (!out || segs < 6 || sides < 4) return false;
    uint32_t vc = (uint32_t)((segs + 1) * (sides + 1));
    uint32_t ic = (uint32_t)(segs * sides * 6);
    out->verts = (ipo_gfx3d_vertex_t *)malloc(vc * sizeof(ipo_gfx3d_vertex_t));
    out->indices = (uint32_t *)malloc(ic * sizeof(uint32_t));
    if (!out->verts || !out->indices) { ipo_gfx3d_mesh_free(out); return false; }
    out->vert_count = vc;
    out->index_count = ic;
    uint32_t vi = 0;
    for (int i = 0; i <= segs; i++) {
        double u = (double)i / (double)segs;
        double theta = u * 2.0 * M_PI;
        double ct = cos(theta), st = sin(theta);
        for (int j = 0; j <= sides; j++) {
            double v = (double)j / (double)sides;
            double phi = v * 2.0 * M_PI;
            double cp = cos(phi), sp = sin(phi);
            double x = (major_r + minor_r * cp) * ct;
            double y = minor_r * sp;
            double z = (major_r + minor_r * cp) * st;
            out->verts[vi].pos = ipo_vec3(x, y, z);
            out->verts[vi].normal = ipo_vec3(cp * ct, sp, cp * st);
            out->verts[vi].u = u * 4.0; out->verts[vi].v = v;
            out->verts[vi].color = (ipo_rgba64f_t){ 1.0, 1.0, 1.0, 1.0 };
            vi++;
        }
    }
    uint32_t ii = 0;
    for (int i = 0; i < segs; i++) {
        for (int j = 0; j < sides; j++) {
            uint32_t a = (uint32_t)(i * (sides + 1) + j);
            uint32_t b = (uint32_t)((i + 1) * (sides + 1) + j);
            out->indices[ii++] = a; out->indices[ii++] = a + 1u; out->indices[ii++] = b;
            out->indices[ii++] = b; out->indices[ii++] = a + 1u; out->indices[ii++] = b + 1u;
        }
    }
    return true;
}

bool ipo_gfx3d_mesh_plane(ipo_gfx3d_mesh_t *out, double hw, double hd, int subdiv) {
    if (!out || subdiv < 1) return false;
    uint32_t vc = (uint32_t)((subdiv + 1) * (subdiv + 1));
    uint32_t ic = (uint32_t)(subdiv * subdiv * 6);
    out->verts = (ipo_gfx3d_vertex_t *)malloc(vc * sizeof(ipo_gfx3d_vertex_t));
    out->indices = (uint32_t *)malloc(ic * sizeof(uint32_t));
    if (!out->verts || !out->indices) { ipo_gfx3d_mesh_free(out); return false; }
    out->vert_count = vc;
    out->index_count = ic;
    uint32_t vi = 0;
    for (int z = 0; z <= subdiv; z++) {
        double tz = (double)z / (double)subdiv;
        for (int x = 0; x <= subdiv; x++) {
            double tx = (double)x / (double)subdiv;
            out->verts[vi].pos = ipo_vec3((tx * 2.0 - 1.0) * hw, 0.0, (tz * 2.0 - 1.0) * hd);
            out->verts[vi].normal = ipo_vec3(0.0, 1.0, 0.0);
            out->verts[vi].u = tx * 6.0; out->verts[vi].v = tz * 6.0;
            out->verts[vi].color = (ipo_rgba64f_t){ 1.0, 1.0, 1.0, 1.0 };
            vi++;
        }
    }
    uint32_t ii = 0;
    for (int z = 0; z < subdiv; z++) {
        for (int x = 0; x < subdiv; x++) {
            uint32_t a = (uint32_t)(z * (subdiv + 1) + x);
            uint32_t b = (uint32_t)((z + 1) * (subdiv + 1) + x);
            out->indices[ii++] = a; out->indices[ii++] = a + 1u; out->indices[ii++] = b;
            out->indices[ii++] = b; out->indices[ii++] = a + 1u; out->indices[ii++] = b + 1u;
        }
    }
    return true;
}

void ipo_gfx3d_mesh_free(ipo_gfx3d_mesh_t *mesh) {
    if (!mesh) return;
    extern void g3d_gpu_invalidate_mesh(const ipo_gfx3d_mesh_t *mesh);
    g3d_gpu_invalidate_mesh(mesh);
    free(mesh->verts);
    free(mesh->indices);
    memset(mesh, 0, sizeof(*mesh));
}

bool ipo_gfx3d_tex_checker(ipo_gfx3d_texture_t *out, int w, int h, int check_px,
                           ipo_rgba64f_t c0, ipo_rgba64f_t c1) {
    if (!out || w <= 0 || h <= 0 || check_px <= 0) return false;
    out->texels = (ipo_rgba64f_t *)malloc((size_t)w * (size_t)h * sizeof(ipo_rgba64f_t));
    if (!out->texels) return false;
    out->width = w; out->height = h;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            bool odd = (((x / check_px) + (y / check_px)) & 1) != 0;
            out->texels[(size_t)y * (size_t)w + x] = odd ? c1 : c0;
        }
    }
    return true;
}

void ipo_gfx3d_tex_free(ipo_gfx3d_texture_t *tex) {
    if (!tex) return;
    extern void g3d_gpu_invalidate_tex(const ipo_gfx3d_texture_t *tex);
    g3d_gpu_invalidate_tex(tex);
    free(tex->texels);
    memset(tex, 0, sizeof(*tex));
}

void ipo_gfx3d_draw_rect2d(ipo_gfx3d_ctx_t *ctx, int x, int y, int w, int h, ipo_rgba64f_t premul_col) {
    extern bool g3d_gpu_draw_rect2d(ipo_gfx3d_ctx_t *ctx, int x, int y, int w, int h, ipo_rgba64f_t premul_col);
    if (g3d_gpu_draw_rect2d(ctx, x, y, w, h, premul_col)) return;
    if (!ctx || !ctx->color_fb || w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = (x + w > ctx->width) ? ctx->width : (x + w);
    int y1 = (y + h > ctx->height) ? ctx->height : (y + h);
    for (int py = y0; py < y1; py++) {
        ipo_rgba64f_t *row = ctx->color_fb + (size_t)py * (size_t)ctx->width;
        for (int px = x0; px < x1; px++) {
            row[px] = ipo_rgba64f_blend_over(row[px], premul_col);
        }
    }
}

void ipo_gfx3d_draw_line2d(ipo_gfx3d_ctx_t *ctx, int x0, int y0, int x1, int y1, ipo_rgba64f_t premul_col) {
    extern bool g3d_gpu_draw_line2d(ipo_gfx3d_ctx_t *ctx, int x0, int y0, int x1, int y1, ipo_rgba64f_t premul_col);
    if (g3d_gpu_draw_line2d(ctx, x0, y0, x1, y1, premul_col)) return;
    if (!ctx || !ctx->color_fb) return;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        if (x0 >= 0 && x0 < ctx->width && y0 >= 0 && y0 < ctx->height) {
            ipo_rgba64f_t *p = &ctx->color_fb[(size_t)y0 * (size_t)ctx->width + x0];
            *p = ipo_rgba64f_blend_over(*p, premul_col);
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* Minimal 5x7 HUD glyph table for public 2D+3D overlay rendering */
static const uint8_t s_hud_font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},{0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},{0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},{0x10,0x08,0x08,0x10,0x08}
};

void ipo_gfx3d_draw_text2d(ipo_gfx3d_ctx_t *ctx, int x, int y, const char *text,
                           ipo_rgba64f_t color, int scale) {
    if (!ctx || !ctx->color_fb || !text) return;
    if (scale < 1) scale = 1;
    int cx = x;
    for (const char *p = text; *p; p++) {
        uint8_t ch = (uint8_t)*p;
        if (ch == '\n') { cx = x; y += 8 * scale; continue; }
        if (ch < 32u || ch > 126u) ch = '?';
        const uint8_t *g = s_hud_font5x7[ch - 32u];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1u << row)) {
                    ipo_gfx3d_draw_rect2d(ctx, cx + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
        cx += 6 * scale;
    }
}

typedef struct {
    const ipo_rgba64f_t *fb;
    uint32_t            *dst;
    int                  w, h;
    ipo_tonemap_op_t     tonemap;
    float                exposure;
} g3d_present_job_t;

static void g3d_pfx_present_xrgb_worker(int tid, int nthreads, void *opaque) {
    const g3d_present_job_t *j = (const g3d_present_job_t *)opaque;
    int y0 = 0, y1 = 0;
    g3d_band_range(tid, nthreads, j->h, &y0, &y1);
    const int w = j->w;
    const float ex = j->exposure;
    const ipo_tonemap_op_t op = j->tonemap;

    for (int y = y0; y < y1; y++) {
        size_t base = (size_t)y * (size_t)w;
        const ipo_rgba64f_t *src = j->fb + base;
        uint32_t *dst = j->dst + base;
        for (int x = 0; x < w; x++) {
            float ch[3] = { (float)src[x].r * ex, (float)src[x].g * ex, (float)src[x].b * ex };
            uint32_t out[3];
            for (int k = 0; k < 3; k++) {
                float v = ch[k];
                float g;
                if (!(v > 0.0f)) {
                    out[k] = 0u;
                    continue;
                }
                if (op == IPO_TONEMAP_ACES) {
                    if (v < 8.0f) {
                        float pos = v * 128.0f;
                        int idx = (int)pos;
                        if (idx >= 1024) g = s_g3d_aces_gamma_lut[1024];
                        else {
                            float f = pos - (float)idx;
                            g = s_g3d_aces_gamma_lut[idx] + (s_g3d_aces_gamma_lut[idx + 1] - s_g3d_aces_gamma_lut[idx]) * f;
                        }
                    } else {
                        g = 1.0f;
                    }
                } else if (op == IPO_TONEMAP_REINHARD) {
                    float t = v / (1.0f + v);
                    g = sqrtf(t > 1.0f ? 1.0f : t);
                } else {
                    float t = (v > 1.0f) ? 1.0f : v;
                    g = sqrtf(t);
                }
                int u = (int)(g * 255.0f + 0.5f);
                out[k] = (uint32_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            }
            dst[x] = (out[0] << 16) | (out[1] << 8) | out[2];
        }
    }
}

int ipo_gfx3d_present(ipo_gfx3d_ctx_t *ctx, wm_window_t *win,
                      ipo_tonemap_op_t tonemap, double exposure) {
    extern int g3d_gpu_present(ipo_gfx3d_ctx_t *ctx, wm_window_t *win,
                               ipo_tonemap_op_t tonemap, double exposure, bool *out_handled);
    bool gpu_handled = false;
    int gpu_rc = g3d_gpu_present(ctx, win, tonemap, exposure, &gpu_handled);
    if (gpu_handled) {
        __atomic_store_n(&s_g3d_pool.hot, 0, __ATOMIC_RELEASE);
        return gpu_rc;
    }
    if (!ctx || !ctx->color_fb) {
        __atomic_store_n(&s_g3d_pool.hot, 0, __ATOMIC_RELEASE);
        return -1;
    }
    if (!g3d_is_postfx_done(ctx)) {
        ipo_gfx3d_apply_postfx(ctx, 0.16, true);
    }
    g3d_init_luts();

    size_t npx = (size_t)ctx->width * (size_t)ctx->height;
    static uint32_t *s_pres_xrgb = NULL;
    static size_t    s_pres_cap  = 0u;
    if (npx > s_pres_cap) {
        uint32_t *np = (uint32_t *)realloc(s_pres_xrgb, npx * sizeof(uint32_t));
        if (np) {
            s_pres_xrgb = np;
            s_pres_cap = npx;
        }
    }
    if (s_pres_xrgb && s_pres_cap >= npx) {
        g3d_present_job_t pj = {
            .fb = ctx->color_fb,
            .dst = s_pres_xrgb,
            .w = ctx->width,
            .h = ctx->height,
            .tonemap = tonemap,
            .exposure = (float)exposure
        };
        if (ctx->height >= 32) g3d_pool_dispatch(g3d_pfx_present_xrgb_worker, &pj);
        else g3d_pfx_present_xrgb_worker(0, 1, &pj);
        __atomic_store_n(&s_g3d_pool.hot, 0, __ATOMIC_RELEASE);

        ipo_gfx_surface_desc_t surf;
        memset(&surf, 0, sizeof(surf));
        surf.struct_size = (uint32_t)sizeof(surf);
        surf.width = (uint32_t)ctx->width;
        surf.height = (uint32_t)ctx->height;
        surf.stride_bytes = (uint32_t)ctx->width * (uint32_t)sizeof(uint32_t);
        surf.format_id = IPO_COLOR_FMT_XRGB8888;
        surf.tonemap_op = IPO_TONEMAP_NONE;
        surf.exposure = 1.0;
        surf.pixels = s_pres_xrgb;
        if (win) return ipo_gfx_present_window(win, &surf);
        return ipo_gfx_present_screen(&surf);
    }

    __atomic_store_n(&s_g3d_pool.hot, 0, __ATOMIC_RELEASE);
    ipo_gfx_surface_desc_t surf;
    memset(&surf, 0, sizeof(surf));
    surf.struct_size = (uint32_t)sizeof(surf);
    surf.width = (uint32_t)ctx->width;
    surf.height = (uint32_t)ctx->height;
    surf.stride_bytes = (uint32_t)ctx->width * (uint32_t)sizeof(ipo_rgba64f_t);
    surf.format_id = IPO_COLOR_FMT_RGBA64F;
    surf.tonemap_op = (uint32_t)tonemap;
    surf.exposure = exposure;
    surf.pixels = ctx->color_fb;
    if (win) return ipo_gfx_present_window(win, &surf);
    return ipo_gfx_present_screen(&surf);
}
