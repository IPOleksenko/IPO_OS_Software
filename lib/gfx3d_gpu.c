/* Hardware VirGL 3D / VirtIO-GPU backend for <ipo/gfx3d.h>.
 * Zero ABI impact on ipo_gfx3d_ctx_t / mesh / texture; falls back cleanly to CPU. */

#include <ipo/gfx3d.h>
#include <ipo/gpu_client.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    const void *verts_ptr;
    uint32_t    vert_count;
    uint32_t    index_count;
    uint64_t    vert_hash;
    uint64_t    idx_hash;
    uint32_t    vbo_res;
    uint32_t    ibo_res;
} g3d_mesh_cache_t;

typedef struct {
    const void *texels_ptr;
    int         width;
    int         height;
    uint64_t    tex_hash;
    uint32_t    tex_res;
} g3d_tex_cache_t;

static uint64_t g3d_fnv1a(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 14695981039346656037ULL;
    size_t step = (len > 4096u) ? (len / 512u) : 1u;
    if (step == 0u) step = 1u;
    for (size_t i = 0u; i < len; i += step) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    if (len > 0u) {
        h ^= (uint64_t)p[len - 1u];
        h *= 1099511628211ULL;
    }
    return h ^ (uint64_t)len;
}

typedef struct {
    ipo_gfx3d_ctx_t  *owner;
    uint32_t          ctx_id;
    uint32_t          hdr_msaa_res;
    uint32_t          depth_msaa_res;
    uint32_t          hdr_res;
    uint32_t          depth_res;
    uint32_t          sdr_res;
    uint32_t          bloom_a_res;
    uint32_t          bloom_b_res;
    uint32_t          env_sharp_res;
    uint32_t          env_soft_res;
    int               rt_w, rt_h;
    int               bloom_w, bloom_h;
    bool              frame_active;

    ipo_gpu_batch_t   batch;
    ipo_gpu_dynbuf_t  dyn_vbo;
    ipo_gpu_vertex_t *verts_2d;
    uint32_t          v2d_tri_count;
    uint32_t          v2d_tri_cap;
    ipo_gpu_vertex_t *lines_2d;
    uint32_t          l2d_vert_count;
    uint32_t          l2d_vert_cap;

    g3d_mesh_cache_t *meshes;
    uint32_t          mesh_count;
    uint32_t          mesh_cap;

    g3d_tex_cache_t  *texs;
    uint32_t          tex_count;
    uint32_t          tex_cap;
} g3d_gpu_state_t;

static g3d_gpu_state_t **s_states = NULL;
static uint32_t          s_state_count = 0u;
static uint32_t          s_state_cap = 0u;

static g3d_gpu_state_t *g3d_find_state(const ipo_gfx3d_ctx_t *ctx, bool create) {
    if (!ctx) return NULL;
    for (uint32_t i = 0u; i < s_state_count; i++) {
        if (s_states[i] && s_states[i]->owner == ctx) return s_states[i];
    }
    if (!create) return NULL;
    if (s_state_count >= s_state_cap) {
        uint32_t ncap = s_state_cap ? (s_state_cap * 2u) : 8u;
        g3d_gpu_state_t **na = (g3d_gpu_state_t **)realloc(s_states, (size_t)ncap * sizeof(g3d_gpu_state_t *));
        if (!na) return NULL;
        s_states = na;
        s_state_cap = ncap;
    }
    g3d_gpu_state_t *st = (g3d_gpu_state_t *)calloc(1u, sizeof(g3d_gpu_state_t));
    if (!st) return NULL;
    st->owner = (ipo_gfx3d_ctx_t *)ctx;
    s_states[s_state_count++] = st;
    return st;
}

static void g3d_destroy_rt(g3d_gpu_state_t *st) {
    if (!st) return;
    if (st->hdr_msaa_res)   { ipo_gpu_res_destroy(st->hdr_msaa_res);   st->hdr_msaa_res = 0u; }
    if (st->depth_msaa_res) { ipo_gpu_res_destroy(st->depth_msaa_res); st->depth_msaa_res = 0u; }
    if (st->hdr_res)        { ipo_gpu_res_destroy(st->hdr_res);        st->hdr_res = 0u; }
    if (st->depth_res)      { ipo_gpu_res_destroy(st->depth_res);      st->depth_res = 0u; }
    if (st->sdr_res)        { ipo_gpu_res_destroy(st->sdr_res);        st->sdr_res = 0u; }
    if (st->bloom_a_res)    { ipo_gpu_res_destroy(st->bloom_a_res);    st->bloom_a_res = 0u; }
    if (st->bloom_b_res)    { ipo_gpu_res_destroy(st->bloom_b_res);    st->bloom_b_res = 0u; }
    st->rt_w = 0;
    st->rt_h = 0;
    st->bloom_w = 0;
    st->bloom_h = 0;
    st->frame_active = false;
}

static void g3d_clear_caches(g3d_gpu_state_t *st) {
    if (!st) return;
    for (uint32_t i = 0u; i < st->mesh_count; i++) {
        if (st->meshes[i].vbo_res) ipo_gpu_res_destroy(st->meshes[i].vbo_res);
        if (st->meshes[i].ibo_res) ipo_gpu_res_destroy(st->meshes[i].ibo_res);
    }
    st->mesh_count = 0u;
    for (uint32_t i = 0u; i < st->tex_count; i++) {
        if (st->texs[i].tex_res) ipo_gpu_res_destroy(st->texs[i].tex_res);
    }
    st->tex_count = 0u;
    if (st->env_sharp_res) { ipo_gpu_res_destroy(st->env_sharp_res); st->env_sharp_res = 0u; }
    if (st->env_soft_res)  { ipo_gpu_res_destroy(st->env_soft_res);  st->env_soft_res = 0u; }
}

static bool g3d_ensure_rt(g3d_gpu_state_t *st, int w, int h) {
    if (!st || w <= 0 || h <= 0) return false;
    if (!st->ctx_id) {
        st->ctx_id = ipo_gpu_ctx_create("ipo_gfx3d");
        if (!st->ctx_id) return false;
    }
    if (!st->env_sharp_res || !st->env_soft_res) {
        ipo_gpu_create_studio_env(st->ctx_id, &st->env_sharp_res, &st->env_soft_res);
    }
    if (!st->hdr_res || (!st->depth_res && !st->depth_msaa_res) || !st->sdr_res ||
        st->rt_w != w || st->rt_h != h) {
        g3d_destroy_rt(st);
        uint32_t ms = ipo_gpu_max_samples();
        (void)ms;
        /* Perceptual FXAA runs cleanly in FS_TONEMAP without 2x llvmpipe MSAA resolve overhead */
        uint32_t samples = 0u;
        if (samples >= 2u) {
            st->hdr_msaa_res   = ipo_gpu_res_create_rt(st->ctx_id, IPO_GPU_FMT_RGBA16F,
                                                       (uint32_t)w, (uint32_t)h,
                                                       IPO_GPU_RES_FLAG_MSAA(samples));
            st->depth_msaa_res = ipo_gpu_res_create_rt(st->ctx_id, IPO_GPU_FMT_Z32F,
                                                       (uint32_t)w, (uint32_t)h,
                                                       IPO_GPU_RES_FLAG_MSAA(samples));
            if (!st->hdr_msaa_res || !st->depth_msaa_res) {
                if (st->hdr_msaa_res)   { ipo_gpu_res_destroy(st->hdr_msaa_res);   st->hdr_msaa_res = 0u; }
                if (st->depth_msaa_res) { ipo_gpu_res_destroy(st->depth_msaa_res); st->depth_msaa_res = 0u; }
                samples = 0u;
            }
        }
        st->hdr_res   = ipo_gpu_res_create_rt(st->ctx_id, IPO_GPU_FMT_RGBA16F,
                                              (uint32_t)w, (uint32_t)h, IPO_GPU_RES_FLAG_SAMPLER);
        if (samples < 2u) {
            st->depth_res = ipo_gpu_res_create_rt(st->ctx_id, IPO_GPU_FMT_Z32F,
                                                  (uint32_t)w, (uint32_t)h, 0u);
        }
        st->sdr_res   = ipo_gpu_res_create_rt(st->ctx_id, IPO_GPU_FMT_BGRX8,
                                              (uint32_t)w, (uint32_t)h, IPO_GPU_RES_FLAG_SAMPLER);
        int bw = (w >= 64) ? (w / 4) : 16;
        int bh = (h >= 64) ? (h / 4) : 16;
        st->bloom_a_res = ipo_gpu_res_create_rt(st->ctx_id, IPO_GPU_FMT_RGBA16F,
                                                (uint32_t)bw, (uint32_t)bh, IPO_GPU_RES_FLAG_SAMPLER);
        st->bloom_b_res = ipo_gpu_res_create_rt(st->ctx_id, IPO_GPU_FMT_RGBA16F,
                                                (uint32_t)bw, (uint32_t)bh, IPO_GPU_RES_FLAG_SAMPLER);
        if (!st->hdr_res || (samples < 2u && !st->depth_res) || !st->sdr_res) {
            g3d_destroy_rt(st);
            return false;
        }
        st->rt_w = w;
        st->rt_h = h;
        st->bloom_w = bw;
        st->bloom_h = bh;
    }
    return true;
}

static void g3d_transpose_d2f(const ipo_mat4_t *cm, float out[16]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = (float)cm->m[c * 4 + r];
        }
    }
}

static bool g3d_push_2d_vert(ipo_gpu_vertex_t **arr, uint32_t *cnt, uint32_t *cap,
                             float nx, float ny, float u, float v, ipo_rgba64f_t col) {
    if (*cnt >= *cap) {
        uint32_t ncap = (*cap) ? (*cap * 2u) : 256u;
        ipo_gpu_vertex_t *na = (ipo_gpu_vertex_t *)realloc(*arr, (size_t)ncap * sizeof(ipo_gpu_vertex_t));
        if (!na) return false;
        *arr = na;
        *cap = ncap;
    }
    ipo_gpu_vertex_t *dv = &(*arr)[(*cnt)++];
    memset(dv, 0, sizeof(*dv));
    dv->px = nx; dv->py = ny; dv->pz = 0.0f;
    dv->u  = u;  dv->v  = v;
    dv->r  = (float)col.r; dv->g = (float)col.g; dv->b = (float)col.b; dv->a = (float)col.a;
    return true;
}

static uint64_t g3d_fast_sig(const void *ptr, size_t bytes) {
    const uint8_t *p = (const uint8_t *)ptr;
    if (!p || bytes == 0u) return 0u;
    if (bytes <= 512u) return g3d_fnv1a(p, bytes);
    uint64_t h = 14695981039346656037ULL ^ (uint64_t)bytes;
    size_t step = bytes / 16u;
    for (size_t s = 0u; s < 16u; s++) {
        size_t off = s * step;
        size_t chunk = (bytes - off >= 32u) ? 32u : (bytes - off);
        for (size_t i = 0u; i < chunk; i++) {
            h ^= (uint64_t)p[off + i];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

static bool g3d_get_mesh_res(g3d_gpu_state_t *st, const ipo_gfx3d_mesh_t *mesh,
                             uint32_t *out_vbo, uint32_t *out_ibo) {
    if (!st || !mesh || !mesh->verts || !mesh->indices ||
        mesh->vert_count == 0u || mesh->index_count == 0u) return false;
    uint64_t vhash = g3d_fast_sig(mesh->verts, (size_t)mesh->vert_count * sizeof(ipo_gfx3d_vertex_t));
    uint64_t ihash = g3d_fast_sig(mesh->indices, (size_t)mesh->index_count * sizeof(uint32_t));
    g3d_mesh_cache_t *reuse = NULL;
    for (uint32_t i = 0u; i < st->mesh_count; i++) {
        g3d_mesh_cache_t *m = &st->meshes[i];
        if (m->verts_ptr == mesh->verts) {
            if (m->vert_count == mesh->vert_count && m->index_count == mesh->index_count &&
                m->vert_hash == vhash && m->idx_hash == ihash && m->vbo_res && m->ibo_res) {
                *out_vbo = m->vbo_res;
                *out_ibo = m->ibo_res;
                return true;
            }
            if (m->vbo_res) ipo_gpu_res_destroy(m->vbo_res);
            if (m->ibo_res) ipo_gpu_res_destroy(m->ibo_res);
            m->vbo_res = 0u; m->ibo_res = 0u;
            reuse = m;
            break;
        }
    }

    ipo_gpu_vertex_t *vtmp = (ipo_gpu_vertex_t *)malloc((size_t)mesh->vert_count * sizeof(ipo_gpu_vertex_t));
    if (!vtmp) return false;
    for (uint32_t i = 0u; i < mesh->vert_count; i++) {
        const ipo_gfx3d_vertex_t *sv = &mesh->verts[i];
        vtmp[i].px = (float)sv->pos.x;    vtmp[i].py = (float)sv->pos.y;    vtmp[i].pz = (float)sv->pos.z;
        vtmp[i].nx = (float)sv->normal.x; vtmp[i].ny = (float)sv->normal.y; vtmp[i].nz = (float)sv->normal.z;
        vtmp[i].u  = (float)sv->u;        vtmp[i].v  = (float)sv->v;
        vtmp[i].r  = (float)sv->color.r;  vtmp[i].g  = (float)sv->color.g;
        vtmp[i].b  = (float)sv->color.b;  vtmp[i].a  = (float)sv->color.a;
    }
    uint32_t vbo = ipo_gpu_res_create_buf(st->ctx_id, IPO_GPU_RES_VERTEX_BUF, vtmp,
                                          mesh->vert_count * (uint32_t)sizeof(ipo_gpu_vertex_t));
    free(vtmp);
    uint32_t ibo = ipo_gpu_res_create_buf(st->ctx_id, IPO_GPU_RES_INDEX_BUF, mesh->indices,
                                          mesh->index_count * (uint32_t)sizeof(uint32_t));
    if (!vbo || !ibo) {
        if (vbo) ipo_gpu_res_destroy(vbo);
        if (ibo) ipo_gpu_res_destroy(ibo);
        return false;
    }

    g3d_mesh_cache_t *ent = reuse;
    if (!ent) {
        if (st->mesh_count >= st->mesh_cap) {
            uint32_t ncap = st->mesh_cap ? (st->mesh_cap * 2u) : 32u;
            g3d_mesh_cache_t *na = (g3d_mesh_cache_t *)realloc(st->meshes, (size_t)ncap * sizeof(g3d_mesh_cache_t));
            if (!na) { ipo_gpu_res_destroy(vbo); ipo_gpu_res_destroy(ibo); return false; }
            st->meshes = na;
            st->mesh_cap = ncap;
        }
        ent = &st->meshes[st->mesh_count++];
    }
    ent->verts_ptr   = mesh->verts;
    ent->vert_count  = mesh->vert_count;
    ent->index_count = mesh->index_count;
    ent->vert_hash   = vhash;
    ent->idx_hash    = ihash;
    ent->vbo_res     = vbo;
    ent->ibo_res     = ibo;
    *out_vbo = vbo;
    *out_ibo = ibo;
    return true;
}

static uint32_t g3d_get_tex_res(g3d_gpu_state_t *st, const ipo_gfx3d_texture_t *tex) {
    if (!st || !tex || !tex->texels || tex->width <= 0 || tex->height <= 0) return 0u;
    size_t npix = (size_t)tex->width * (size_t)tex->height;
    uint64_t thash = g3d_fast_sig(tex->texels, npix * sizeof(*tex->texels));
    g3d_tex_cache_t *reuse = NULL;
    for (uint32_t i = 0u; i < st->tex_count; i++) {
        g3d_tex_cache_t *tc = &st->texs[i];
        if (tc->texels_ptr == tex->texels) {
            if (tc->width == tex->width && tc->height == tex->height &&
                tc->tex_hash == thash && tc->tex_res) {
                return tc->tex_res;
            }
            if (tc->tex_res) ipo_gpu_res_destroy(tc->tex_res);
            tc->tex_res = 0u;
            reuse = tc;
            break;
        }
    }
    float *fbuf = (float *)malloc(npix * 4u * sizeof(float));
    if (!fbuf) return 0u;
    for (size_t i = 0u; i < npix; i++) {
        fbuf[i * 4u + 0u] = (float)tex->texels[i].r;
        fbuf[i * 4u + 1u] = (float)tex->texels[i].g;
        fbuf[i * 4u + 2u] = (float)tex->texels[i].b;
        fbuf[i * 4u + 3u] = (float)tex->texels[i].a;
    }
    uint32_t tid = ipo_gpu_res_create_tex2d(st->ctx_id, IPO_GPU_FMT_RGBA32F,
                                            (uint32_t)tex->width, (uint32_t)tex->height,
                                            fbuf, (uint32_t)tex->width * 16u);
    free(fbuf);
    if (!tid) return 0u;
    g3d_tex_cache_t *ent = reuse;
    if (!ent) {
        if (st->tex_count >= st->tex_cap) {
            uint32_t ncap = st->tex_cap ? (st->tex_cap * 2u) : 16u;
            g3d_tex_cache_t *na = (g3d_tex_cache_t *)realloc(st->texs, (size_t)ncap * sizeof(g3d_tex_cache_t));
            if (!na) { ipo_gpu_res_destroy(tid); return 0u; }
            st->texs = na;
            st->tex_cap = ncap;
        }
        ent = &st->texs[st->tex_count++];
    }
    ent->texels_ptr = tex->texels;
    ent->width      = tex->width;
    ent->height     = tex->height;
    ent->tex_hash   = thash;
    ent->tex_res    = tid;
    return tid;
}

void g3d_gpu_destroy_ctx(ipo_gfx3d_ctx_t *ctx) {
    if (!ctx) return;
    for (uint32_t i = 0u; i < s_state_count; i++) {
        if (s_states[i] && s_states[i]->owner == ctx) {
            g3d_gpu_state_t *st = s_states[i];
            g3d_clear_caches(st);
            g3d_destroy_rt(st);
            ipo_gpu_dynbuf_destroy(&st->dyn_vbo);
            ipo_gpu_batch_free(&st->batch);
            free(st->verts_2d);
            free(st->lines_2d);
            free(st->meshes);
            free(st->texs);
            if (st->ctx_id) ipo_gpu_ctx_destroy(st->ctx_id);
            free(st);
            s_states[i] = s_states[--s_state_count];
            return;
        }
    }
}

void g3d_gpu_invalidate_rt(ipo_gfx3d_ctx_t *ctx) {
    g3d_gpu_state_t *st = g3d_find_state(ctx, false);
    if (st) g3d_destroy_rt(st);
}

void g3d_gpu_invalidate_mesh(const ipo_gfx3d_mesh_t *mesh) {
    if (!mesh || !mesh->verts) return;
    for (uint32_t s = 0u; s < s_state_count; s++) {
        g3d_gpu_state_t *st = s_states[s];
        if (!st) continue;
        for (uint32_t i = 0u; i < st->mesh_count; ) {
            if (st->meshes[i].verts_ptr == mesh->verts) {
                if (st->meshes[i].vbo_res) ipo_gpu_res_destroy(st->meshes[i].vbo_res);
                if (st->meshes[i].ibo_res) ipo_gpu_res_destroy(st->meshes[i].ibo_res);
                st->meshes[i] = st->meshes[--st->mesh_count];
            } else {
                i++;
            }
        }
    }
}

void g3d_gpu_invalidate_tex(const ipo_gfx3d_texture_t *tex) {
    if (!tex || !tex->texels) return;
    for (uint32_t s = 0u; s < s_state_count; s++) {
        g3d_gpu_state_t *st = s_states[s];
        if (!st) continue;
        for (uint32_t i = 0u; i < st->tex_count; ) {
            if (st->texs[i].texels_ptr == tex->texels) {
                if (st->texs[i].tex_res) ipo_gpu_res_destroy(st->texs[i].tex_res);
                st->texs[i] = st->texs[--st->tex_count];
            } else {
                i++;
            }
        }
    }
}

bool g3d_gpu_clear(ipo_gfx3d_ctx_t *ctx, ipo_rgba64f_t sky_top, ipo_rgba64f_t sky_horizon, double depth) {
    if (!ctx || !ipo_gpu_3d_available()) {
        g3d_gpu_state_t *st = g3d_find_state(ctx, false);
        if (st) st->frame_active = false;
        return false;
    }
    g3d_gpu_state_t *st = g3d_find_state(ctx, true);
    if (!st || !g3d_ensure_rt(st, ctx->width, ctx->height)) {
        if (st) st->frame_active = false;
        return false;
    }

    ipo_gpu_batch_reset(&st->batch);
    st->v2d_tri_count = 0u;
    st->l2d_vert_count = 0u;
    st->frame_active = true;

    uint32_t rt_c = st->hdr_msaa_res ? st->hdr_msaa_res : st->hdr_res;
    uint32_t rt_d = st->depth_msaa_res ? st->depth_msaa_res : st->depth_res;
    ipo_gpu_cmd_set_target(&st->batch, rt_c, rt_d,
                           (uint32_t)ctx->width, (uint32_t)ctx->height);
    ipo_gpu_cmd_set_viewport(&st->batch, 0.0f, 0.0f, (float)ctx->width, (float)ctx->height, 0.0f, 1.0f);
    float rgba[4] = { (float)sky_horizon.r, (float)sky_horizon.g, (float)sky_horizon.b, 1.0f };
    ipo_gpu_cmd_clear(&st->batch, IPO_GPU_CLEAR_COLOR | IPO_GPU_CLEAR_DEPTH, rgba, (float)depth);

    /* Draw 8-band non-linear sky gradient matching CPU curve in dyn_vbo slot 0..47 */
    for (int band = 0; band < 8; band++) {
        double t0 = (double)band / 8.0;
        double t1 = (double)(band + 1) / 8.0;
        double b0 = sqrt(t0 * sqrt(t0));
        double b1 = sqrt(t1 * sqrt(t1));
        ipo_rgba64f_t c0 = {
            sky_top.r * (1.0 - b0) + sky_horizon.r * b0,
            sky_top.g * (1.0 - b0) + sky_horizon.g * b0,
            sky_top.b * (1.0 - b0) + sky_horizon.b * b0,
            1.0
        };
        ipo_rgba64f_t c1 = {
            sky_top.r * (1.0 - b1) + sky_horizon.r * b1,
            sky_top.g * (1.0 - b1) + sky_horizon.g * b1,
            sky_top.b * (1.0 - b1) + sky_horizon.b * b1,
            1.0
        };
        float y0 = 1.0f - (float)(t0 * 2.0);
        float y1 = 1.0f - (float)(t1 * 2.0);
        g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, -1.0f, y0, 0.0f, (float)t0, c0);
        g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, -1.0f, y1, 0.0f, (float)t1, c1);
        g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap,  1.0f, y0, 1.0f, (float)t0, c0);
        g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap,  1.0f, y0, 1.0f, (float)t0, c0);
        g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, -1.0f, y1, 0.0f, (float)t1, c1);
        g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap,  1.0f, y1, 1.0f, (float)t1, c1);
    }
    /* First 48 vertices of verts_2d are sky gradient */
    ipo_gpu_cmd_set_pipeline(&st->batch, IPO_GPU_PROG_SOLID2D,
                             IPO_GPU_CULL_NONE, true, false, false,
                             IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
    ipo_gpu_cmd_draw(&st->batch, 0u, 0u, 0u, IPO_GPU_PRIM_TRIANGLES, 0u, 48u, 0u, 0u);

    ctx->tris_submitted = 0;
    ctx->tris_drawn = 0;
    return true;
}

bool g3d_gpu_clear_depth(ipo_gfx3d_ctx_t *ctx, double depth) {
    g3d_gpu_state_t *st = g3d_find_state(ctx, false);
    if (!st || !st->frame_active) return false;
    float rgba[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ipo_gpu_cmd_clear(&st->batch, IPO_GPU_CLEAR_DEPTH, rgba, (float)depth);
    return true;
}

bool g3d_gpu_is_frame_active(const ipo_gfx3d_ctx_t *ctx) {
    const g3d_gpu_state_t *st = g3d_find_state(ctx, false);
    return (st && st->frame_active);
}

bool g3d_gpu_draw_mesh(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                       const ipo_mat4_t *model, const ipo_gfx3d_material_t *mat) {
    g3d_gpu_state_t *st = g3d_find_state(ctx, false);
    if (!st || !st->frame_active || !mesh || !model || !mat) return false;

    uint32_t vbo = 0u, ibo = 0u;
    if (!g3d_get_mesh_res(st, mesh, &vbo, &ibo)) return false;

    uint32_t tex_res = mat->albedo_tex ? g3d_get_tex_res(st, mat->albedo_tex) : 0u;

    uint8_t cull = IPO_GPU_CULL_NONE;
    if (ctx->cull_mode == IPO_GFX3D_CULL_BACK)       cull = IPO_GPU_CULL_BACK;
    else if (ctx->cull_mode == IPO_GFX3D_CULL_FRONT) cull = IPO_GPU_CULL_FRONT;

    bool translucent = (mat->albedo.a >= 0.0 && mat->albedo.a < 0.999);
    ipo_gpu_cmd_set_pipeline(&st->batch, IPO_GPU_PROG_PBR3D,
                             cull, true,
                             ctx->depth_test, ctx->depth_write,
                             IPO_GPU_CMP_LESS,
                             translucent ? IPO_GPU_BLEND_ALPHA : IPO_GPU_BLEND_NONE, false);

    ipo_mat4_t vp = ipo_mat4_mul(&ctx->proj, &ctx->view);
    ipo_mat4_t mvp = ipo_mat4_mul(&vp, model);
    ipo_mat4_t id = ipo_mat4_identity();

    float vs_c[60];
    g3d_transpose_d2f(&mvp,  &vs_c[0]);
    g3d_transpose_d2f(model, &vs_c[16]);
    g3d_transpose_d2f(&id,   &vs_c[32]);
    {
        float *nm = &vs_c[48];
        memset(nm, 0, 12u * sizeof(float));
        nm[0] = 1.0f; nm[5] = 1.0f; nm[10] = 1.0f;
        float a = vs_c[16], b = vs_c[17], c0 = vs_c[18];
        float d = vs_c[20], e = vs_c[21], f0 = vs_c[22];
        float g = vs_c[24], h = vs_c[25], k  = vs_c[26];
        float c00 = e * k - f0 * h, c01 = f0 * g - d * k, c02 = d * h - e * g;
        float c10 = c0 * h - b * k, c11 = a * k - c0 * g, c12 = b * g - a * h;
        float c20 = b * f0 - c0 * e, c21 = c0 * d - a * f0, c22 = a * e - b * d;
        float det = a * c00 + b * c01 + c0 * c02;
        if (fabsf(det) > 1e-8f) {
            float inv = 1.0f / det;
            nm[0] = c00 * inv; nm[1] = c01 * inv; nm[2]  = c02 * inv;
            nm[4] = c10 * inv; nm[5] = c11 * inv; nm[6]  = c12 * inv;
            nm[8] = c20 * inv; nm[9] = c21 * inv; nm[10] = c22 * inv;
        }
    }
    ipo_gpu_cmd_set_constants(&st->batch, 0u, vs_c, 60u);

    ipo_gpu_cmd_bind_tex(&st->batch, 0u, tex_res, true, true);
    ipo_gpu_cmd_bind_tex(&st->batch, 1u, 0u, false, false);
    bool have_env = (ctx->enable_env_reflection && st->env_sharp_res && st->env_soft_res);
    ipo_gpu_cmd_bind_tex(&st->batch, 2u, have_env ? st->env_sharp_res : 0u, true, false);
    ipo_gpu_cmd_bind_tex(&st->batch, 3u, have_env ? st->env_soft_res  : 0u, true, false);

    float metal = (float)(mat->metallic < 0.0 ? 0.0 : (mat->metallic > 1.0 ? 1.0 : mat->metallic));
    float rough = (float)(mat->roughness < 0.04 ? 0.04 : (mat->roughness > 1.0 ? 1.0 : mat->roughness));
    float ao = (mat->ao > 0.0) ? (float)mat->ao : 1.0f;
    float refl = (ctx->enable_env_reflection && (mat->reflectivity > 0.0 || metal > 0.0 || mat->clearcoat > 0.0))
               ? (float)mat->reflectivity * (1.0f - rough * 0.45f)
               : 0.0f;

    float fs_c[23u * 4u];
    memset(fs_c, 0, sizeof(fs_c));
    fs_c[0] = (float)ctx->cam_pos.x; fs_c[1] = (float)ctx->cam_pos.y; fs_c[2] = (float)ctx->cam_pos.z;
    fs_c[3] = refl;

    fs_c[4] = metal; fs_c[5] = rough; fs_c[6] = (float)mat->clearcoat; fs_c[7] = ao;
    fs_c[8] = (float)mat->albedo.r; fs_c[9] = (float)mat->albedo.g; fs_c[10] = (float)mat->albedo.b;
    fs_c[11] = (tex_res != 0u) ? 1.0f : 0.0f;

    fs_c[12] = (float)mat->emissive.r; fs_c[13] = (float)mat->emissive.g; fs_c[14] = (float)mat->emissive.b;
    fs_c[15] = 0.0f; /* shadow off */

    fs_c[16] = (float)ctx->ambient_sky.r;    fs_c[17] = (float)ctx->ambient_sky.g;    fs_c[18] = (float)ctx->ambient_sky.b;
    fs_c[20] = (float)ctx->ambient_ground.r; fs_c[21] = (float)ctx->ambient_ground.g; fs_c[22] = (float)ctx->ambient_ground.b;
    fs_c[23] = 0.002f;

    int active = 0;
    for (uint32_t li = 0u; li < ctx->lights_cap && active < 8; li++) {
        if (!ctx->lights[li].enabled) continue;
        int b = 24 + active * 8;
        fs_c[b + 0] = (float)ctx->lights[li].pos_or_dir.x;
        fs_c[b + 1] = (float)ctx->lights[li].pos_or_dir.y;
        fs_c[b + 2] = (float)ctx->lights[li].pos_or_dir.z;
        fs_c[b + 3] = ctx->lights[li].is_point ? 1.0f : 0.0f;
        fs_c[b + 4] = (float)ctx->lights[li].color.r;
        fs_c[b + 5] = (float)ctx->lights[li].color.g;
        fs_c[b + 6] = (float)ctx->lights[li].color.b;
        fs_c[b + 7] = 0.0f;
        active++;
    }
    fs_c[19] = (float)active;
    if (translucent) {
        float a_ov = (float)mat->albedo.a;
        if (a_ov < 0.01f) a_ov = 0.01f;
        fs_c[31] = a_ov; /* CONST[7].w alpha override in FS_PBR3D */
    }
    fs_c[88] = have_env ? 1.0f : 0.0f; /* CONST[22].x: Dual Octahedral Studio HDRI */
    ipo_gpu_cmd_set_constants(&st->batch, 1u, fs_c, 92u);
    ipo_gpu_cmd_draw(&st->batch, vbo, ibo, 4u, IPO_GPU_PRIM_TRIANGLES, 0u, mesh->index_count, 0u, 0u);

    uint32_t tc = mesh->index_count / 3u;
    ctx->tris_submitted += tc;
    ctx->tris_drawn     += tc;
    return true;
}

static ipo_rgba64f_t g3d_premul_to_sdr_srgb(ipo_rgba64f_t c) {
    double a = c.a;
    if (a < 0.0) a = 0.0; else if (a > 1.0) a = 1.0;
    double inv_a = (a > 1e-6 && a < 1.0) ? (1.0 / a) : 1.0;
    double rgb[3] = { c.r * inv_a, c.g * inv_a, c.b * inv_a };
    for (int k = 0; k < 3; k++) {
        double v = rgb[k];
        if (v < 0.0) v = 0.0;
        v = (v * (2.51 * v + 0.03)) / (v * (2.43 * v + 0.59) + 0.14);
        if (v < 0.0) v = 0.0; else if (v > 1.0) v = 1.0;
        rgb[k] = pow(v, 1.0 / 2.2) * a;
    }
    return (ipo_rgba64f_t){ rgb[0], rgb[1], rgb[2], a };
}

bool g3d_gpu_draw_shadow_projection(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                                    const ipo_mat4_t *model, ipo_vec3_t light_dir,
                                    double plane_y, double opacity) {
    g3d_gpu_state_t *st = g3d_find_state(ctx, false);
    if (!st || !st->frame_active || !mesh || !model) return false;
    if (light_dir.y <= 0.05) return true;

    uint32_t vbo = 0u, ibo = 0u;
    if (!g3d_get_mesh_res(st, mesh, &vbo, &ibo)) return false;

    static const double k_shd_ox[5] = { 0.0,  0.048, -0.048,  0.030, -0.030 };
    static const double k_shd_oz[5] = { 0.0,  0.030, -0.030, -0.048,  0.048 };
    static const double k_shd_yo[5] = { 0.0020, 0.0040, 0.0060, 0.0080, 0.0100 };
    double clamped_op = (opacity < 0.0) ? 0.0 : (opacity > 0.95 ? 0.95 : opacity);
    float layer_alpha = (float)(1.0 - pow(1.0 - clamped_op, 0.2));
    if (layer_alpha < 0.02f) layer_alpha = 0.02f;

    ipo_mat4_t vp = ipo_mat4_mul(&ctx->proj, &ctx->view);
    ipo_mat4_t id = ipo_mat4_identity();

    ipo_gpu_cmd_set_pipeline(&st->batch, IPO_GPU_PROG_STD3D,
                             IPO_GPU_CULL_NONE, true,
                             true, true, IPO_GPU_CMP_LESS,
                             IPO_GPU_BLEND_ALPHA, false);
    ipo_gpu_cmd_bind_tex(&st->batch, 0u, 0u, false, false);

    float fs_c[24];
    memset(fs_c, 0, sizeof(fs_c));
    fs_c[1]  = 1.0f;
    fs_c[19] = layer_alpha;
    ipo_gpu_cmd_set_constants(&st->batch, 1u, fs_c, 24u);

    for (int tap = 0; tap < 5; tap++) {
        ipo_vec3_t ld = ipo_vec3_norm(ipo_vec3(light_dir.x + k_shd_ox[tap], light_dir.y, light_dir.z + k_shd_oz[tap]));
        ipo_mat4_t proj_m = ipo_mat4_identity();
        proj_m.m[4]  = -ld.x / ld.y;
        proj_m.m[5]  = 0.0;
        proj_m.m[6]  = -ld.z / ld.y;
        proj_m.m[13] = plane_y + k_shd_yo[tap];

        ipo_mat4_t sh_world = ipo_mat4_mul(&proj_m, model);
        ipo_mat4_t mvp = ipo_mat4_mul(&vp, &sh_world);
        float vs_c[48];
        g3d_transpose_d2f(&mvp, &vs_c[0]);
        g3d_transpose_d2f(&sh_world, &vs_c[16]);
        g3d_transpose_d2f(&id, &vs_c[32]);
        ipo_gpu_cmd_set_constants(&st->batch, 0u, vs_c, 44u);
        ipo_gpu_cmd_draw(&st->batch, vbo, ibo, 4u, IPO_GPU_PRIM_TRIANGLES, 0u, mesh->index_count, 0u, 0u);
    }
    return true;
}

bool g3d_gpu_draw_rect2d(ipo_gfx3d_ctx_t *ctx, int x, int y, int w, int h, ipo_rgba64f_t premul_col) {
    g3d_gpu_state_t *st = g3d_find_state(ctx, false);
    if (!st || !st->frame_active || ctx->width <= 0 || ctx->height <= 0) return false;
    if (w <= 0 || h <= 0) return true;

    ipo_rgba64f_t sc = g3d_premul_to_sdr_srgb(premul_col);
    float x0 =  ((float)x / (float)ctx->width) * 2.0f - 1.0f;
    float x1 =  ((float)(x + w) / (float)ctx->width) * 2.0f - 1.0f;
    float y0 = 1.0f - ((float)y / (float)ctx->height) * 2.0f;
    float y1 = 1.0f - ((float)(y + h) / (float)ctx->height) * 2.0f;

    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, x0, y0, 0.0f, 0.0f, sc);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, x0, y1, 0.0f, 1.0f, sc);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, x1, y0, 1.0f, 0.0f, sc);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, x1, y0, 1.0f, 0.0f, sc);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, x0, y1, 0.0f, 1.0f, sc);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, x1, y1, 1.0f, 1.0f, sc);
    return true;
}

bool g3d_gpu_draw_line2d(ipo_gfx3d_ctx_t *ctx, int x0, int y0, int x1, int y1, ipo_rgba64f_t premul_col) {
    g3d_gpu_state_t *st = g3d_find_state(ctx, false);
    if (!st || !st->frame_active || ctx->width <= 0 || ctx->height <= 0) return false;

    ipo_rgba64f_t sc = g3d_premul_to_sdr_srgb(premul_col);
    float nx0 = ((float)x0 + 0.5f) / (float)ctx->width  * 2.0f - 1.0f;
    float nx1 = ((float)x1 + 0.5f) / (float)ctx->width  * 2.0f - 1.0f;
    float ny0 = 1.0f - ((float)y0 + 0.5f) / (float)ctx->height * 2.0f;
    float ny1 = 1.0f - ((float)y1 + 0.5f) / (float)ctx->height * 2.0f;

    g3d_push_2d_vert(&st->lines_2d, &st->l2d_vert_count, &st->l2d_vert_cap, nx0, ny0, 0.0f, 0.0f, sc);
    g3d_push_2d_vert(&st->lines_2d, &st->l2d_vert_count, &st->l2d_vert_cap, nx1, ny1, 0.0f, 0.0f, sc);
    return true;
}

int g3d_gpu_present(ipo_gfx3d_ctx_t *ctx, wm_window_t *win,
                    ipo_tonemap_op_t tonemap, double exposure, bool *out_handled) {
    if (out_handled) *out_handled = false;
    g3d_gpu_state_t *st = g3d_find_state(ctx, false);
    if (!st || !st->frame_active) return 0;
    if (out_handled) *out_handled = true;

    /* Resolve 4x MSAA -> 1x hdr_res, run Bloom on 3D highlights, then composite sharp 2D HUD */
    if (st->hdr_msaa_res) {
        ipo_gpu_cmd_blit(&st->batch, st->hdr_res, st->hdr_msaa_res,
                         0, 0, ctx->width, ctx->height,
                         0, 0, ctx->width, ctx->height,
                         false, false);
    }

    uint32_t tm_base_v = st->v2d_tri_count + st->l2d_vert_count;
    ipo_rgba64f_t white = { 1.0, 1.0, 1.0, 1.0 };
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, -1.0f,  1.0f, 0.0f, 0.0f, white);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, -1.0f, -1.0f, 0.0f, 1.0f, white);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap,  1.0f,  1.0f, 1.0f, 0.0f, white);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap,  1.0f,  1.0f, 1.0f, 0.0f, white);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap, -1.0f, -1.0f, 0.0f, 1.0f, white);
    g3d_push_2d_vert(&st->verts_2d, &st->v2d_tri_count, &st->v2d_tri_cap,  1.0f, -1.0f, 1.0f, 1.0f, white);

    uint32_t rect_tri_verts = (st->v2d_tri_count > 54u) ? (st->v2d_tri_count - 54u) : 0u;
    uint32_t line_base_v = 48u + rect_tri_verts;
    uint32_t total_v = st->v2d_tri_count + st->l2d_vert_count;
    ipo_gpu_vertex_t *packed = (ipo_gpu_vertex_t *)malloc((size_t)total_v * sizeof(ipo_gpu_vertex_t));
    if (!packed) { st->frame_active = false; return -1; }
    uint32_t head_cnt = 48u + rect_tri_verts;
    memcpy(packed, st->verts_2d, (size_t)head_cnt * sizeof(ipo_gpu_vertex_t));
    if (st->l2d_vert_count > 0u) {
        memcpy(packed + head_cnt, st->lines_2d, (size_t)st->l2d_vert_count * sizeof(ipo_gpu_vertex_t));
    }
    memcpy(packed + head_cnt + st->l2d_vert_count, st->verts_2d + head_cnt, 6u * sizeof(ipo_gpu_vertex_t));

    bool up_ok = ipo_gpu_dynbuf_upload(st->ctx_id, &st->dyn_vbo, IPO_GPU_RES_VERTEX_BUF,
                                       packed, total_v * (uint32_t)sizeof(ipo_gpu_vertex_t));
    free(packed);
    if (!up_ok) { st->frame_active = false; return -1; }

    bool use_bloom = (st->bloom_a_res && st->bloom_b_res && st->bloom_w > 0 && st->bloom_h > 0);
    if (use_bloom) {
        float bx_c[4] = { 1.15f, 1.35f, 0.72f, 8.0f };
        ipo_gpu_cmd_set_target(&st->batch, st->bloom_a_res, 0u, (uint32_t)st->bloom_w, (uint32_t)st->bloom_h);
        ipo_gpu_cmd_set_viewport(&st->batch, 0.0f, 0.0f, (float)st->bloom_w, (float)st->bloom_h, 0.0f, 1.0f);
        ipo_gpu_cmd_set_pipeline(&st->batch, IPO_GPU_PROG_BLOOM_EXTRACT,
                                 IPO_GPU_CULL_NONE, true, false, false, IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
        ipo_gpu_cmd_set_constants(&st->batch, 1u, bx_c, 4u);
        ipo_gpu_cmd_bind_tex(&st->batch, 0u, st->hdr_res, true, false);
        ipo_gpu_cmd_draw(&st->batch, st->dyn_vbo.res_id, 0u, 0u, IPO_GPU_PRIM_TRIANGLES, tm_base_v, 6u, 0u, 0u);

        float bh_c[4] = { 1.0f / (float)st->bloom_w, 0.0f, 0.0f, 0.0f };
        ipo_gpu_cmd_set_target(&st->batch, st->bloom_b_res, 0u, (uint32_t)st->bloom_w, (uint32_t)st->bloom_h);
        ipo_gpu_cmd_set_pipeline(&st->batch, IPO_GPU_PROG_BLOOM_BLUR,
                                 IPO_GPU_CULL_NONE, true, false, false, IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
        ipo_gpu_cmd_set_constants(&st->batch, 1u, bh_c, 4u);
        ipo_gpu_cmd_bind_tex(&st->batch, 0u, st->bloom_a_res, true, false);
        ipo_gpu_cmd_draw(&st->batch, st->dyn_vbo.res_id, 0u, 0u, IPO_GPU_PRIM_TRIANGLES, tm_base_v, 6u, 0u, 0u);

        float bv_c[4] = { 0.0f, 1.0f / (float)st->bloom_h, 0.0f, 0.0f };
        ipo_gpu_cmd_set_target(&st->batch, st->bloom_a_res, 0u, (uint32_t)st->bloom_w, (uint32_t)st->bloom_h);
        ipo_gpu_cmd_set_constants(&st->batch, 1u, bv_c, 4u);
        ipo_gpu_cmd_bind_tex(&st->batch, 0u, st->bloom_b_res, true, false);
        ipo_gpu_cmd_draw(&st->batch, st->dyn_vbo.res_id, 0u, 0u, IPO_GPU_PRIM_TRIANGLES, tm_base_v, 6u, 0u, 0u);
    }

    /* PostFX Tonemap pass: hdr_res + bloom + perceptual FXAA + vignette -> sdr_res (BGRX8) */
    ipo_gpu_cmd_set_target(&st->batch, st->sdr_res, 0u, (uint32_t)ctx->width, (uint32_t)ctx->height);
    ipo_gpu_cmd_set_viewport(&st->batch, 0.0f, 0.0f, (float)ctx->width, (float)ctx->height, 0.0f, 1.0f);
    ipo_gpu_cmd_set_pipeline(&st->batch, IPO_GPU_PROG_TONEMAP,
                             IPO_GPU_CULL_NONE, true, false, false,
                             IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
    float tm_c[8] = {
        (exposure > 0.0) ? (float)exposure : 1.0f,
        (float)tonemap,
        1.0f, /* sRGB encode */
        0.0f,
        use_bloom ? 0.30f : 0.0f,
        1.0f / (float)ctx->width,
        1.0f / (float)ctx->height,
        0.22f
    };
    ipo_gpu_cmd_set_constants(&st->batch, 1u, tm_c, 8u);
    ipo_gpu_cmd_bind_tex(&st->batch, 0u, st->hdr_res, true, false);
    ipo_gpu_cmd_bind_tex(&st->batch, 1u, use_bloom ? st->bloom_a_res : 0u, true, false);
    ipo_gpu_cmd_draw(&st->batch, st->dyn_vbo.res_id, 0u, 0u, IPO_GPU_PRIM_TRIANGLES, tm_base_v, 6u, 0u, 0u);

    /* Composite needle-sharp 2D HUD rects/lines directly onto sdr_res after FXAA/tonemap */
    if (rect_tri_verts > 0u || st->l2d_vert_count > 0u) {
        if (rect_tri_verts > 0u) {
            ipo_gpu_cmd_set_pipeline(&st->batch, IPO_GPU_PROG_SOLID2D,
                                     IPO_GPU_CULL_NONE, true, false, false,
                                     IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_PREMUL, false);
            ipo_gpu_cmd_draw(&st->batch, 0u, 0u, 0u, IPO_GPU_PRIM_TRIANGLES, 48u, rect_tri_verts, 0u, 0u);
        }
        if (st->l2d_vert_count > 0u) {
            ipo_gpu_cmd_set_pipeline(&st->batch, IPO_GPU_PROG_SOLID2D,
                                     IPO_GPU_CULL_NONE, true, false, false,
                                     IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_PREMUL, false);
            ipo_gpu_cmd_draw(&st->batch, 0u, 0u, 0u, IPO_GPU_PRIM_LINES, line_base_v, st->l2d_vert_count, 0u, 0u);
        }
    }

    /* Patch placeholder dyn_vbo slots (vbo_res == 0) */
    for (uint32_t i = 0u; i < st->batch.cmd_count; i++) {
        if (st->batch.cmds[i].op == IPO_GPU_CMD_DRAW &&
            st->batch.cmds[i].u.draw.vbo_res == 0u) {
            st->batch.cmds[i].u.draw.vbo_res = st->dyn_vbo.res_id;
        }
    }

    if (win) {
        ipo_gpu_cmd_present_window(&st->batch, st->sdr_res, win, (uint32_t)ctx->width, (uint32_t)ctx->height);
    } else {
        ipo_gpu_cmd_present_screen(&st->batch, st->sdr_res, (uint32_t)ctx->width, (uint32_t)ctx->height);
    }

    int rc = ipo_gpu_batch_flush(&st->batch, st->ctx_id);
    st->frame_active = false;
    return rc;
}
