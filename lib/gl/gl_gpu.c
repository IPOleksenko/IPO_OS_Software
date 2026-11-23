/* Hardware VirGL 3D / VirtIO-GPU backend for IPO OpenGL (`lib/gl`).
 * Batches draw primitives per frame and submits them to VirtIO-GPU VirGL.
 * When VirGL GPU is unavailable or disabled at runtime, falls back transparently to CPU. */

#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <GL/gl_math.h>
#include <ipo/gpu_client.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint8_t gl_map_cmp_func(GLenum func) {
    switch (func) {
        case GL_NEVER:    return IPO_GPU_CMP_NEVER;
        case GL_LESS:     return IPO_GPU_CMP_LESS;
        case GL_EQUAL:    return IPO_GPU_CMP_EQUAL;
        case GL_LEQUAL:   return IPO_GPU_CMP_LEQUAL;
        case GL_GREATER:  return IPO_GPU_CMP_GREATER;
        case GL_NOTEQUAL: return IPO_GPU_CMP_NOTEQ;
        case GL_GEQUAL:   return IPO_GPU_CMP_GEQUAL;
        case GL_ALWAYS:
        default:          return IPO_GPU_CMP_ALWAYS;
    }
}

static uint8_t gl_map_bf(GLenum f) {
    switch (f) {
        case GL_ZERO: return IPO_GPU_BF_ZERO;
        case GL_ONE: return IPO_GPU_BF_ONE;
        case GL_SRC_COLOR: return IPO_GPU_BF_SRC_COLOR;
        case GL_ONE_MINUS_SRC_COLOR: return IPO_GPU_BF_INV_SRC_COLOR;
        case GL_SRC_ALPHA: return IPO_GPU_BF_SRC_ALPHA;
        case GL_ONE_MINUS_SRC_ALPHA: return IPO_GPU_BF_INV_SRC_ALPHA;
        case GL_DST_ALPHA: return IPO_GPU_BF_DST_ALPHA;
        case GL_ONE_MINUS_DST_ALPHA: return IPO_GPU_BF_INV_DST_ALPHA;
        case GL_DST_COLOR: return IPO_GPU_BF_DST_COLOR;
        case GL_ONE_MINUS_DST_COLOR: return IPO_GPU_BF_INV_DST_COLOR;
        case GL_SRC_ALPHA_SATURATE: return IPO_GPU_BF_SRC_ALPHA_SATURATE;
        default: return IPO_GPU_BF_ONE;
    }
}

static uint8_t gl_map_blend_mode(GLContext *ctx) {
    if (!ctx || !ctx->blend_enabled) return IPO_GPU_BLEND_NONE;
    if (ctx->blend_sfactor == GL_SRC_ALPHA && ctx->blend_dfactor == GL_ONE) {
        return IPO_GPU_BLEND_ADDITIVE;
    }
    if (ctx->blend_sfactor == GL_ONE && ctx->blend_dfactor == GL_ONE_MINUS_SRC_ALPHA) {
        return IPO_GPU_BLEND_PREMUL;
    }
    if (ctx->blend_sfactor == GL_DST_COLOR && ctx->blend_dfactor == GL_ZERO) {
        return IPO_GPU_BLEND_MULTIPLY;
    }
    return IPO_GPU_BLEND_ALPHA;
}

static void gl_gpu_transpose_mat4(const GLfloat col_major[16], float row_major[16]) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            row_major[r * 4 + c] = col_major[c * 4 + r];
        }
    }
}

static void gl_gpu_calc_norm_mat3x4(const GLfloat mv[16], float out3x4[12]) {
    /* Upper 3x3 cofactor/det of column-major MV gives (M^-1)^T rows directly:
     * row r, col c of inv_trans(M) = cofactor(M, row=r, col=c) / det */
    float a00 = mv[0], a01 = mv[4], a02 = mv[8];
    float a10 = mv[1], a11 = mv[5], a12 = mv[9];
    float a20 = mv[2], a21 = mv[6], a22 = mv[10];

    float c00 = a11 * a22 - a12 * a21;
    float c01 = -(a10 * a22 - a12 * a20);
    float c02 = a10 * a21 - a11 * a20;

    float det = a00 * c00 + a01 * c01 + a02 * c02;
    if (fabsf(det) < 1e-8f) {
        out3x4[0] = 1.0f; out3x4[1] = 0.0f; out3x4[2] = 0.0f; out3x4[3] = 0.0f;
        out3x4[4] = 0.0f; out3x4[5] = 1.0f; out3x4[6] = 0.0f; out3x4[7] = 0.0f;
        out3x4[8] = 0.0f; out3x4[9] = 0.0f; out3x4[10] = 1.0f; out3x4[11] = 0.0f;
        return;
    }
    float idet = 1.0f / det;
    out3x4[0]  = c00 * idet;
    out3x4[1]  = c01 * idet;
    out3x4[2]  = c02 * idet;
    out3x4[3]  = 0.0f;

    out3x4[4]  = -(a01 * a22 - a02 * a21) * idet;
    out3x4[5]  =  (a00 * a22 - a02 * a20) * idet;
    out3x4[6]  = -(a00 * a21 - a01 * a20) * idet;
    out3x4[7]  = 0.0f;

    out3x4[8]  =  (a01 * a12 - a02 * a11) * idet;
    out3x4[9]  = -(a00 * a12 - a02 * a10) * idet;
    out3x4[10] =  (a00 * a11 - a01 * a10) * idet;
    out3x4[11] = 0.0f;
}

static bool gl_gpu_ensure_ctx_and_rt(GLContext *ctx) {
    if (!ctx || ctx->width <= 0 || ctx->height <= 0) return false;
    if (!ctx->gpu_ctx_id) {
        ctx->gpu_ctx_id = ipo_gpu_ctx_create("ipo_gl");
        if (!ctx->gpu_ctx_id) return false;
    }
    if (!ctx->gpu_quad_vbo.res_id) {
        ipo_gpu_vertex_t q[6];
        memset(q, 0, sizeof(q));
        #define SET_Q(i, x0, y0, u0, v0) do { \
            q[i].px = (x0); q[i].py = (y0); q[i].u = (u0); q[i].v = (v0); \
            q[i].r = 1.0f; q[i].g = 1.0f; q[i].b = 1.0f; q[i].a = 1.0f; \
        } while (0)
        SET_Q(0, -1.0f,  1.0f, 0.0f, 0.0f);
        SET_Q(1, -1.0f, -1.0f, 0.0f, 1.0f);
        SET_Q(2,  1.0f,  1.0f, 1.0f, 0.0f);
        SET_Q(3,  1.0f,  1.0f, 1.0f, 0.0f);
        SET_Q(4, -1.0f, -1.0f, 0.0f, 1.0f);
        SET_Q(5,  1.0f, -1.0f, 1.0f, 1.0f);
        #undef SET_Q
        ipo_gpu_dynbuf_upload(ctx->gpu_ctx_id, &ctx->gpu_quad_vbo, IPO_GPU_RES_VERTEX_BUF,
                              q, (uint32_t)sizeof(q));
    }
    if (!ctx->gpu_color_res || (!ctx->gpu_depth_res && !ctx->gpu_msaa_depth_res) ||
        ctx->gpu_rt_w != ctx->width || ctx->gpu_rt_h != ctx->height) {
        gl_gpu_invalidate_rt(ctx);
        uint32_t ms = ipo_gpu_max_samples();
        uint32_t samples = (ms >= 4u) ? 4u : ((ms >= 2u) ? 2u : 0u);
        if (samples >= 2u) {
            ctx->gpu_msaa_color_res = ipo_gpu_res_create_rt(ctx->gpu_ctx_id, IPO_GPU_FMT_BGRX8,
                                                            (uint32_t)ctx->width, (uint32_t)ctx->height,
                                                            IPO_GPU_RES_FLAG_MSAA(samples));
            ctx->gpu_msaa_depth_res = ipo_gpu_res_create_rt(ctx->gpu_ctx_id, IPO_GPU_FMT_Z32F,
                                                            (uint32_t)ctx->width, (uint32_t)ctx->height,
                                                            IPO_GPU_RES_FLAG_MSAA(samples));
            if (!ctx->gpu_msaa_color_res || !ctx->gpu_msaa_depth_res) {
                if (ctx->gpu_msaa_color_res) { ipo_gpu_res_destroy(ctx->gpu_msaa_color_res); ctx->gpu_msaa_color_res = 0u; }
                if (ctx->gpu_msaa_depth_res) { ipo_gpu_res_destroy(ctx->gpu_msaa_depth_res); ctx->gpu_msaa_depth_res = 0u; }
                samples = 0u;
            }
        }
        ctx->gpu_color_res = ipo_gpu_res_create_rt(ctx->gpu_ctx_id, IPO_GPU_FMT_BGRX8,
                                                   (uint32_t)ctx->width, (uint32_t)ctx->height,
                                                   IPO_GPU_RES_FLAG_SAMPLER);
        if (samples < 2u) {
            ctx->gpu_depth_res = ipo_gpu_res_create_rt(ctx->gpu_ctx_id, IPO_GPU_FMT_Z32F,
                                                       (uint32_t)ctx->width, (uint32_t)ctx->height, 0u);
        }
        ctx->gpu_postfx_res = ipo_gpu_res_create_rt(ctx->gpu_ctx_id, IPO_GPU_FMT_BGRX8,
                                                    (uint32_t)ctx->width, (uint32_t)ctx->height,
                                                    IPO_GPU_RES_FLAG_SAMPLER);
        if (!ctx->gpu_color_res || (samples < 2u && !ctx->gpu_depth_res)) {
            gl_gpu_invalidate_rt(ctx);
            return false;
        }
        ctx->gpu_rt_w = ctx->width;
        ctx->gpu_rt_h = ctx->height;
    }
    return true;
}

static gl_texture_t *gl_gpu_find_tex(GLContext *ctx, GLuint id) {
    if (!ctx || id == 0u) return NULL;
    for (int i = 0; i < ctx->texture_count; i++) {
        if (ctx->textures[i] && ctx->textures[i]->id == id && ctx->textures[i]->in_use) {
            return ctx->textures[i];
        }
    }
    return NULL;
}

static void gl_gpu_defer_res_destroy(GLContext *ctx, uint32_t res_id);

static uint32_t gl_gpu_sync_bound_tex(GLContext *ctx, bool *out_linear, bool *out_repeat) {
    *out_linear = true;
    *out_repeat = true;
    if (!ctx || !ctx->texture_2d_enabled || ctx->current_texture_2d == 0u) return 0u;
    gl_texture_t *t = gl_gpu_find_tex(ctx, ctx->current_texture_2d);
    if (!t || !t->pixels || t->width <= 0 || t->height <= 0) return 0u;

    *out_linear = (t->mag_filter == GL_LINEAR || t->min_filter == GL_LINEAR);
    *out_repeat = (t->wrap_s == GL_REPEAT || t->wrap_t == GL_REPEAT);

    if (t->gpu_res_id && t->gpu_dirty) {
        gl_gpu_defer_res_destroy(ctx, t->gpu_res_id);
        t->gpu_res_id = 0u;
    }
    if (!t->gpu_res_id) {
        t->gpu_res_id = ipo_gpu_res_create_tex2d(ctx->gpu_ctx_id, IPO_GPU_FMT_BGRA8,
                                                 (uint32_t)t->width, (uint32_t)t->height,
                                                 t->pixels, (uint32_t)t->width * 4u);
        t->gpu_dirty = false;
    }
    return t->gpu_res_id;
}

static bool gl_gpu_push_vertex(GLContext *ctx, const gl_vertex_t *gv,
                               const GLfloat *norm_override, const GLfloat *col_override) {
    float w = gv->obj[3];
    if (w <= 0.0f) return false;
    if (ctx->gpu_vert_count >= ctx->gpu_vert_cap) {
        uint32_t ncap = ctx->gpu_vert_cap ? (ctx->gpu_vert_cap * 2u) : 2048u;
        ipo_gpu_vertex_t *na = (ipo_gpu_vertex_t *)realloc(ctx->gpu_verts, (size_t)ncap * sizeof(ipo_gpu_vertex_t));
        if (!na) return false;
        ctx->gpu_verts = na;
        ctx->gpu_vert_cap = ncap;
    }
    ipo_gpu_vertex_t *dv = &ctx->gpu_verts[ctx->gpu_vert_count++];
    if (w != 1.0f && fabsf(w) > 1e-7f) {
        float inv_w = 1.0f / w;
        dv->px = gv->obj[0] * inv_w;
        dv->py = gv->obj[1] * inv_w;
        dv->pz = gv->obj[2] * inv_w;
    } else {
        dv->px = gv->obj[0];
        dv->py = gv->obj[1];
        dv->pz = gv->obj[2];
    }
    const GLfloat *n = norm_override ? norm_override : gv->norm;
    const GLfloat *c = col_override  ? col_override  : gv->color;
    dv->nx = n[0]; dv->ny = n[1]; dv->nz = n[2];
    dv->u  = gv->tex[0]; dv->v = gv->tex[1];
    dv->r  = c[0]; dv->g = c[1]; dv->b = c[2]; dv->a = c[3];
    return true;
}

static void gl_gpu_defer_res_destroy(GLContext *ctx, uint32_t res_id) {
    if (!ctx || res_id == 0u) return;
    if (ctx->gpu_pending_destroy_cnt >= ctx->gpu_pending_destroy_cap) {
        size_t ncap = ctx->gpu_pending_destroy_cap ? (ctx->gpu_pending_destroy_cap * 2u) : 8u;
        uint32_t *na = (uint32_t *)realloc(ctx->gpu_pending_destroy_res, ncap * sizeof(uint32_t));
        if (!na) {
            ipo_gpu_res_destroy(res_id);
            return;
        }
        ctx->gpu_pending_destroy_res = na;
        ctx->gpu_pending_destroy_cap = ncap;
    }
    ctx->gpu_pending_destroy_res[ctx->gpu_pending_destroy_cnt++] = res_id;
}

static void gl_gpu_flush_deferred_destroy(GLContext *ctx) {
    if (!ctx || !ctx->gpu_pending_destroy_res) return;
    for (size_t i = 0u; i < ctx->gpu_pending_destroy_cnt; i++) {
        if (ctx->gpu_pending_destroy_res[i]) {
            ipo_gpu_res_destroy(ctx->gpu_pending_destroy_res[i]);
        }
    }
    ctx->gpu_pending_destroy_cnt = 0u;
}

static void gl_gpu_patch_and_flush(GLContext *ctx) {
    if (!ctx || !ctx->gpu_ctx_id) return;
    if (ctx->gpu_vert_count > 0u) {
        uint32_t vbytes = ctx->gpu_vert_count * (uint32_t)sizeof(ipo_gpu_vertex_t);
        if (!ipo_gpu_dynbuf_upload(ctx->gpu_ctx_id, &ctx->gpu_vbo, IPO_GPU_RES_VERTEX_BUF,
                                   ctx->gpu_verts, vbytes)) {
            ipo_gpu_batch_reset(&ctx->gpu_batch);
            ctx->gpu_vert_count = 0u;
            gl_gpu_flush_deferred_destroy(ctx);
            return;
        }
        for (uint32_t i = 0u; i < ctx->gpu_batch.cmd_count; i++) {
            if (ctx->gpu_batch.cmds[i].op == IPO_GPU_CMD_DRAW &&
                ctx->gpu_batch.cmds[i].u.draw.vbo_res == 0u) {
                ctx->gpu_batch.cmds[i].u.draw.vbo_res = ctx->gpu_vbo.res_id;
            }
        }
    }
    ipo_gpu_batch_flush(&ctx->gpu_batch, ctx->gpu_ctx_id);
    ctx->gpu_vert_count = 0u;
    gl_gpu_flush_deferred_destroy(ctx);
}

bool gl_gpu_is_active(void) {
    return ipo_gpu_3d_available();
}

void gl_gpu_invalidate_rt(GLContext *ctx) {
    if (!ctx) return;
    if (ctx->gpu_msaa_color_res) {
        ipo_gpu_res_destroy(ctx->gpu_msaa_color_res);
        ctx->gpu_msaa_color_res = 0u;
    }
    if (ctx->gpu_msaa_depth_res) {
        ipo_gpu_res_destroy(ctx->gpu_msaa_depth_res);
        ctx->gpu_msaa_depth_res = 0u;
    }
    if (ctx->gpu_color_res) {
        ipo_gpu_res_destroy(ctx->gpu_color_res);
        ctx->gpu_color_res = 0u;
    }
    if (ctx->gpu_depth_res) {
        ipo_gpu_res_destroy(ctx->gpu_depth_res);
        ctx->gpu_depth_res = 0u;
    }
    if (ctx->gpu_postfx_res) {
        ipo_gpu_res_destroy(ctx->gpu_postfx_res);
        ctx->gpu_postfx_res = 0u;
    }
    ctx->gpu_rt_w = 0;
    ctx->gpu_rt_h = 0;
    ctx->gpu_frame_active = false;
    ctx->gpu_dirty_content = false;
}

void gl_gpu_destroy_texture(GLContext *ctx, gl_texture_t *tex) {
    if (!tex) return;
    if (tex->gpu_res_id) {
        if (ctx && ctx->gpu_frame_active) {
            gl_gpu_defer_res_destroy(ctx, tex->gpu_res_id);
        } else {
            ipo_gpu_res_destroy(tex->gpu_res_id);
        }
        tex->gpu_res_id = 0u;
    }
    tex->gpu_dirty = false;
}

void gl_gpu_destroy_context_res(GLContext *ctx) {
    if (!ctx) return;
    gl_gpu_invalidate_rt(ctx);
    gl_gpu_flush_deferred_destroy(ctx);
    if (ctx->gpu_pending_destroy_res) {
        free(ctx->gpu_pending_destroy_res);
        ctx->gpu_pending_destroy_res = NULL;
        ctx->gpu_pending_destroy_cap = 0u;
    }
    ipo_gpu_dynbuf_destroy(&ctx->gpu_vbo);
    ipo_gpu_dynbuf_destroy(&ctx->gpu_quad_vbo);
    ipo_gpu_batch_free(&ctx->gpu_batch);
    if (ctx->gpu_verts) {
        free(ctx->gpu_verts);
        ctx->gpu_verts = NULL;
    }
    ctx->gpu_vert_count = 0u;
    ctx->gpu_vert_cap = 0u;
    if (ctx->gpu_ctx_id) {
        ipo_gpu_ctx_destroy(ctx->gpu_ctx_id);
        ctx->gpu_ctx_id = 0u;
    }
}

bool gl_gpu_try_clear(GLContext *ctx, GLbitfield mask) {
    if (!ctx) return false;
    int enabled_lights = 0;
    if (ctx->lighting_enabled && ctx->lights) {
        for (int i = 0; i < ctx->light_capacity; i++) {
            if (ctx->lights[i].enabled) enabled_lights++;
        }
    }
    bool want_gpu = ipo_gpu_3d_available() && !ctx->stencil_test_enabled && !ctx->logic_op_enabled &&
                    enabled_lights <= 8;
    if (!want_gpu) {
        ctx->gpu_frame_active = false;
        ctx->gpu_dirty_content = false;
        ipo_gpu_batch_reset(&ctx->gpu_batch);
        ctx->gpu_vert_count = 0u;
        return false;
    }

    bool partial_scissor = ctx->scissor_enabled &&
        !(ctx->scissor_x == 0 && ctx->scissor_y == 0 &&
          ctx->scissor_w >= ctx->width && ctx->scissor_h >= ctx->height);
    if ((mask & GL_STENCIL_BUFFER_BIT) || ctx->stencil_test_enabled || partial_scissor) {
        if (ctx->gpu_dirty_content) gl_gpu_sync_readback(ctx);
        ctx->gpu_frame_active = false;
        return false;
    }

    if (!gl_gpu_ensure_ctx_and_rt(ctx)) {
        ctx->gpu_frame_active = false;
        return false;
    }

    uint32_t cmask = 0u;
    if (mask & GL_COLOR_BUFFER_BIT) cmask |= IPO_GPU_CLEAR_COLOR;
    if (mask & GL_DEPTH_BUFFER_BIT) cmask |= IPO_GPU_CLEAR_DEPTH;

    if (!ctx->gpu_frame_active ||
        (cmask == (IPO_GPU_CLEAR_COLOR | IPO_GPU_CLEAR_DEPTH) && ctx->gpu_vert_count == 0u)) {
        ipo_gpu_batch_reset(&ctx->gpu_batch);
        ctx->gpu_vert_count = 0u;
        uint32_t rt_c = ctx->gpu_msaa_color_res ? ctx->gpu_msaa_color_res : ctx->gpu_color_res;
        uint32_t rt_d = ctx->gpu_msaa_depth_res ? ctx->gpu_msaa_depth_res : ctx->gpu_depth_res;
        ipo_gpu_cmd_set_target(&ctx->gpu_batch, rt_c, rt_d,
                               (uint32_t)ctx->width, (uint32_t)ctx->height);
        ipo_gpu_cmd_set_viewport(&ctx->gpu_batch, 0.0f, 0.0f, (float)ctx->width, (float)ctx->height, 0.0f, 1.0f);
    }
    ipo_gpu_cmd_set_scissor(&ctx->gpu_batch, false, 0, 0, 0, 0);

    if (cmask != 0u) {
        float rgba[4] = { ctx->clear_color[0], ctx->clear_color[1], ctx->clear_color[2], ctx->clear_color[3] };
        ipo_gpu_cmd_clear(&ctx->gpu_batch, cmask, rgba, (float)ctx->clear_depth);
    }

    ctx->gpu_frame_active = true;
    ctx->gpu_dirty_content = true;
    return true;
}

bool gl_gpu_try_draw_primitive(GLContext *ctx) {
    if (!ctx || !ctx->gpu_frame_active || ctx->vert_buf.count <= 0) return false;

    int n = ctx->vert_buf.count;
    const gl_vertex_t *v = ctx->vert_buf.data;
    uint32_t start_v = ctx->gpu_vert_count;
    uint32_t prim_mode = IPO_GPU_PRIM_TRIANGLES;
    bool flat = (ctx->shade_model == GL_FLAT);

    switch (ctx->prim_mode) {
        case GL_POINTS:
            prim_mode = IPO_GPU_PRIM_POINTS;
            for (int i = 0; i < n; i++) {
                if (!gl_gpu_push_vertex(ctx, &v[i], NULL, NULL)) return false;
            }
            break;

        case GL_LINES:
            prim_mode = IPO_GPU_PRIM_LINES;
            for (int i = 0; i + 1 < n; i += 2) {
                const GLfloat *fn = flat ? v[i + 1].norm  : NULL;
                const GLfloat *fc = flat ? v[i + 1].color : NULL;
                if (!gl_gpu_push_vertex(ctx, &v[i], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 1], fn, fc)) return false;
            }
            break;

        case GL_LINE_STRIP:
        case GL_LINE_LOOP:
            prim_mode = IPO_GPU_PRIM_LINES;
            for (int i = 0; i + 1 < n; i++) {
                const GLfloat *fn = flat ? v[i + 1].norm  : NULL;
                const GLfloat *fc = flat ? v[i + 1].color : NULL;
                if (!gl_gpu_push_vertex(ctx, &v[i], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 1], fn, fc)) return false;
            }
            if (ctx->prim_mode == GL_LINE_LOOP && n >= 2) {
                const GLfloat *fn = flat ? v[0].norm  : NULL;
                const GLfloat *fc = flat ? v[0].color : NULL;
                if (!gl_gpu_push_vertex(ctx, &v[n - 1], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[0], fn, fc)) return false;
            }
            break;

        case GL_TRIANGLES:
            for (int i = 0; i + 2 < n; i += 3) {
                const GLfloat *fn = flat ? v[i + 2].norm  : NULL;
                const GLfloat *fc = flat ? v[i + 2].color : NULL;
                if (!gl_gpu_push_vertex(ctx, &v[i],     fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 1], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 2], fn, fc)) return false;
            }
            break;

        case GL_QUADS:
            for (int i = 0; i + 3 < n; i += 4) {
                const GLfloat *fn = flat ? v[i + 3].norm  : NULL;
                const GLfloat *fc = flat ? v[i + 3].color : NULL;
                if (!gl_gpu_push_vertex(ctx, &v[i],     fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 1], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 2], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i],     fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 2], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 3], fn, fc)) return false;
            }
            break;

        case GL_QUAD_STRIP:
            for (int i = 0; i + 3 < n; i += 2) {
                const GLfloat *fn = flat ? v[i + 3].norm  : NULL;
                const GLfloat *fc = flat ? v[i + 3].color : NULL;
                if (!gl_gpu_push_vertex(ctx, &v[i],     fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 1], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 3], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i],     fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 3], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 2], fn, fc)) return false;
            }
            break;

        case GL_TRIANGLE_STRIP:
            for (int i = 0; i + 2 < n; i++) {
                int i0 = (i & 1) ? (i + 1) : i;
                int i1 = (i & 1) ? i : (i + 1);
                int i2 = i + 2;
                const GLfloat *fn = flat ? v[i2].norm  : NULL;
                const GLfloat *fc = flat ? v[i2].color : NULL;
                if (!gl_gpu_push_vertex(ctx, &v[i0], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i1], fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i2], fn, fc)) return false;
            }
            break;

        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            for (int i = 1; i + 1 < n; i++) {
                const GLfloat *fn = flat ? v[i + 1].norm  : NULL;
                const GLfloat *fc = flat ? v[i + 1].color : NULL;
                if (!gl_gpu_push_vertex(ctx, &v[0],     fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i],     fn, fc)) return false;
                if (!gl_gpu_push_vertex(ctx, &v[i + 1], fn, fc)) return false;
            }
            break;

        default:
            ctx->gpu_vert_count = start_v;
            return false;
    }

    uint32_t emit_cnt = ctx->gpu_vert_count - start_v;
    if (emit_cnt == 0u) return true;

    /* Viewport & Scissor (convert OpenGL bottom-up Y to top-down framebuffer Y) */
    int top_vp_y = ctx->height - (ctx->vp_y + ctx->vp_h);
    ipo_gpu_cmd_set_viewport(&ctx->gpu_batch,
                             (float)ctx->vp_x, (float)top_vp_y,
                             (float)ctx->vp_w, (float)ctx->vp_h,
                             (float)ctx->depth_near, (float)ctx->depth_far);
    int top_sc_y = ctx->height - (ctx->scissor_y + ctx->scissor_h);
    ipo_gpu_cmd_set_scissor(&ctx->gpu_batch, ctx->scissor_enabled != 0,
                            ctx->scissor_x, top_sc_y,
                            ctx->scissor_w, ctx->scissor_h);

    /* Pipeline */
    uint8_t cull = IPO_GPU_CULL_NONE;
    if (ctx->cull_face_enabled) {
        cull = (ctx->cull_face_mode == GL_FRONT) ? IPO_GPU_CULL_FRONT : IPO_GPU_CULL_BACK;
    }
    uint8_t beq = IPO_GPU_BEQ_ADD;
    if (ctx->blend_equation == GL_FUNC_SUBTRACT) beq = IPO_GPU_BEQ_SUB;
    else if (ctx->blend_equation == GL_FUNC_REVERSE_SUBTRACT) beq = IPO_GPU_BEQ_REV_SUB;
    else if (ctx->blend_equation == GL_MIN) beq = IPO_GPU_BEQ_MIN;
    else if (ctx->blend_equation == GL_MAX) beq = IPO_GPU_BEQ_MAX;

    uint8_t bmode = ctx->blend_enabled ? IPO_GPU_BLEND_CUSTOM : IPO_GPU_BLEND_NONE;
    uint8_t bsrc  = ctx->blend_enabled ? gl_map_bf(ctx->blend_sfactor) : IPO_GPU_BF_ONE;
    uint8_t bdst  = ctx->blend_enabled ? gl_map_bf(ctx->blend_dfactor) : IPO_GPU_BF_ZERO;
    ipo_gpu_cmd_set_pipeline_ex(&ctx->gpu_batch, IPO_GPU_PROG_STD3D,
                                cull, ctx->front_face == GL_CCW,
                                ctx->depth_test_enabled != 0,
                                ctx->depth_mask != 0,
                                gl_map_cmp_func(ctx->depth_func),
                                bmode, beq, bsrc, bdst,
                                ctx->polygon_mode == GL_LINE,
                                ctx->point_size, ctx->line_width);

    /* Vertex Shader constants: MVP (4x4), MV (4x4), NormalMatrix (3x4) = 44 floats */
    GLfloat id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    const GLfloat *mv = gl_matrix_current(&ctx->modelview_stack);
    const GLfloat *pj = gl_matrix_current(&ctx->projection_stack);
    if (!mv) mv = id;
    if (!pj) pj = id;

    GLfloat mvp[16];
    gl_matrix_mult(mvp, pj, mv);

    float vs_c[44];
    gl_gpu_transpose_mat4(mvp, &vs_c[0]);
    gl_gpu_transpose_mat4(mv,  &vs_c[16]);
    gl_gpu_calc_norm_mat3x4(mv, &vs_c[32]);
    ipo_gpu_cmd_set_constants(&ctx->gpu_batch, 0u, vs_c, 44u);

    /* Fragment Shader constants (FS_STD3D) */
    bool linear_tex = true, repeat_tex = true;
    uint32_t tex_res = gl_gpu_sync_bound_tex(ctx, &linear_tex, &repeat_tex);
    ipo_gpu_cmd_bind_tex(&ctx->gpu_batch, 0u, tex_res, linear_tex, repeat_tex);

    int active_lights = 0;
    if (ctx->lighting_enabled && ctx->lights) {
        for (int i = 0; i < ctx->light_capacity; i++) {
            if (ctx->lights[i].enabled) active_lights++;
        }
    }
    if (active_lights > 8) active_lights = 8;
    uint32_t fs_floats = 156u;
    float *fs_c = (float *)calloc((size_t)fs_floats, sizeof(float));
    if (!fs_c) return false;
    /* CONST[38]: (alpha_func_mode, fog_mode, fog_density, tex_env_mode) */
    float af_mode = 0.0f;
    switch (ctx->alpha_func) {
        case GL_NEVER:    af_mode = 1.0f; break;
        case GL_LESS:     af_mode = 2.0f; break;
        case GL_EQUAL:    af_mode = 3.0f; break;
        case GL_LEQUAL:   af_mode = 4.0f; break;
        case GL_NOTEQUAL: af_mode = 5.0f; break;
        case GL_GEQUAL:   af_mode = 6.0f; break;
        case GL_ALWAYS:   af_mode = 7.0f; break;
        default:          af_mode = 0.0f; break; /* GL_GREATER */
    }
    float fg_mode = 0.0f;
    if (ctx->fog_mode == GL_EXP) fg_mode = 1.0f;
    else if (ctx->fog_mode == GL_EXP2) fg_mode = 2.0f;
    float te_mode = 0.0f;
    if (ctx->tex_env_mode == GL_REPLACE) te_mode = 1.0f;
    else if (ctx->tex_env_mode == GL_DECAL) te_mode = 2.0f;
#ifdef GL_ADD
    else if (ctx->tex_env_mode == GL_ADD) te_mode = 3.0f;
#endif
    fs_c[152] = af_mode;
    fs_c[153] = fg_mode;
    fs_c[154] = ctx->fog_density;
    fs_c[155] = te_mode;
    fs_c[0] = (tex_res != 0u) ? 1.0f : 0.0f;
    fs_c[1] = ctx->lighting_enabled ? 1.0f : 0.0f;
    fs_c[2] = ctx->fog_enabled ? 1.0f : 0.0f;
    fs_c[3] = ctx->alpha_test_enabled ? 1.0f : 0.0f;

    float fog_span = ctx->fog_end - ctx->fog_start;
    fs_c[4] = ctx->fog_start;
    fs_c[5] = (fabsf(fog_span) > 1e-6f) ? (1.0f / fog_span) : 1.0f;
    fs_c[6] = ctx->alpha_ref;
    fs_c[7] = ctx->color_material_enabled ? 1.0f : 0.0f;

    fs_c[8]  = ctx->fog_color[0];
    fs_c[9]  = ctx->fog_color[1];
    fs_c[10] = ctx->fog_color[2];
    fs_c[11] = ctx->fog_color[3];

    /* CONST[3] multiplies CONST[4] (or vertex color if COLOR_MATERIAL) inside FS_STD3D */
    float md_r = ctx->front_material.diffuse[0], md_g = ctx->front_material.diffuse[1], md_b = ctx->front_material.diffuse[2];
    fs_c[12] = (md_r > 1e-4f) ? (ctx->front_material.ambient[0] * 0.2f + ctx->front_material.emission[0]) / md_r : 0.2f;
    fs_c[13] = (md_g > 1e-4f) ? (ctx->front_material.ambient[1] * 0.2f + ctx->front_material.emission[1]) / md_g : 0.2f;
    fs_c[14] = (md_b > 1e-4f) ? (ctx->front_material.ambient[2] * 0.2f + ctx->front_material.emission[2]) / md_b : 0.2f;
    fs_c[15] = ctx->front_material.shininess;

    fs_c[16] = md_r; fs_c[17] = md_g; fs_c[18] = md_b; fs_c[19] = ctx->front_material.diffuse[3];
    fs_c[20] = ctx->front_material.specular[0];
    fs_c[21] = ctx->front_material.specular[1];
    fs_c[22] = ctx->front_material.specular[2];

    int written_lights = 0;
    if (ctx->lighting_enabled && ctx->lights) {
        for (int i = 0; i < ctx->light_capacity && written_lights < active_lights; i++) {
            const gl_light_t *l = &ctx->lights[i];
            if (!l->enabled) continue;
            int base = 24 + written_lights * 16;
            fs_c[base + 0] = l->position[0];
            fs_c[base + 1] = l->position[1];
            fs_c[base + 2] = l->position[2];
            fs_c[base + 3] = l->position[3];

            fs_c[base + 4] = l->diffuse[0];
            fs_c[base + 5] = l->diffuse[1];
            fs_c[base + 6] = l->diffuse[2];
            fs_c[base + 7] = (l->constant_attenuation > 0.0f) ? l->constant_attenuation : 1.0f;

            fs_c[base + 8]  = l->specular[0];
            fs_c[base + 9]  = l->specular[1];
            fs_c[base + 10] = l->specular[2];
            fs_c[base + 11] = l->linear_attenuation;

            fs_c[base + 12] = l->ambient[0];
            fs_c[base + 13] = l->ambient[1];
            fs_c[base + 14] = l->ambient[2];
            fs_c[base + 15] = l->quadratic_attenuation;
            written_lights++;
        }
    }
    fs_c[23] = (float)written_lights;
    ipo_gpu_cmd_set_constants(&ctx->gpu_batch, 1u, fs_c, fs_floats);
    free(fs_c);

    ipo_gpu_cmd_draw(&ctx->gpu_batch, 0u, 0u, 0u, prim_mode, start_v, emit_cnt, 0u, 0u);
    ctx->gpu_dirty_content = true;
    return true;
}

static uint32_t gl_gpu_resolve_output(GLContext *ctx) {
    if (!ctx) return 0u;
    if (ctx->gpu_msaa_color_res) {
        ipo_gpu_cmd_blit(&ctx->gpu_batch, ctx->gpu_color_res, ctx->gpu_msaa_color_res,
                         0, 0, ctx->width, ctx->height,
                         0, 0, ctx->width, ctx->height,
                         false, false);
    }
    uint32_t out_res = ctx->gpu_color_res;
    if (ctx->gpu_postfx_res && ctx->gpu_quad_vbo.res_id) {
        ipo_gpu_cmd_set_target(&ctx->gpu_batch, ctx->gpu_postfx_res, 0u,
                               (uint32_t)ctx->width, (uint32_t)ctx->height);
        ipo_gpu_cmd_set_viewport(&ctx->gpu_batch, 0.0f, 0.0f, (float)ctx->width, (float)ctx->height, 0.0f, 1.0f);
        ipo_gpu_cmd_set_scissor(&ctx->gpu_batch, false, 0, 0, 0, 0);
        ipo_gpu_cmd_set_pipeline(&ctx->gpu_batch, IPO_GPU_PROG_TONEMAP,
                                 IPO_GPU_CULL_NONE, true, false, false, IPO_GPU_CMP_ALWAYS, IPO_GPU_BLEND_NONE, false);
        float pf[8] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f / (float)ctx->width, 1.0f / (float)ctx->height, 0.0f
        };
        ipo_gpu_cmd_set_constants(&ctx->gpu_batch, 1u, pf, 8u);
        ipo_gpu_cmd_bind_tex(&ctx->gpu_batch, 0u, ctx->gpu_color_res, true, false);
        ipo_gpu_cmd_bind_tex(&ctx->gpu_batch, 1u, 0u, false, false);
        ipo_gpu_cmd_draw(&ctx->gpu_batch, ctx->gpu_quad_vbo.res_id, 0u, 0u,
                         IPO_GPU_PRIM_TRIANGLES, 0u, 6u, 0u, 0u);
        out_res = ctx->gpu_postfx_res;
    }
    return out_res;
}

void gl_gpu_flush(GLContext *ctx) {
    if (!ctx || !ctx->gpu_frame_active || !ctx->gpu_ctx_id || ctx->gpu_batch.cmd_count == 0u) return;
    gl_gpu_patch_and_flush(ctx);
    uint32_t rt_c = ctx->gpu_msaa_color_res ? ctx->gpu_msaa_color_res : ctx->gpu_color_res;
    uint32_t rt_d = ctx->gpu_msaa_depth_res ? ctx->gpu_msaa_depth_res : ctx->gpu_depth_res;
    ipo_gpu_cmd_set_target(&ctx->gpu_batch, rt_c, rt_d, (uint32_t)ctx->width, (uint32_t)ctx->height);
    int top_vp_y = ctx->height - (ctx->vp_y + ctx->vp_h);
    ipo_gpu_cmd_set_viewport(&ctx->gpu_batch, (float)ctx->vp_x, (float)top_vp_y,
                             (float)ctx->vp_w, (float)ctx->vp_h,
                             (float)ctx->depth_near, (float)ctx->depth_far);
}

bool gl_gpu_sync_readback(GLContext *ctx) {
    if (!ctx || !ctx->gpu_dirty_content || !ctx->gpu_ctx_id || !ctx->gpu_color_res) return false;
    uint32_t out_res = gl_gpu_resolve_output(ctx);
    ipo_gpu_cmd_readback(&ctx->gpu_batch, out_res, 0u, 0u,
                         (uint32_t)ctx->width, (uint32_t)ctx->height,
                         ctx->color_buffer, (uint32_t)ctx->width * 4u);
    gl_gpu_patch_and_flush(ctx);
    ctx->gpu_dirty_content = false;
    if (ctx->gpu_frame_active) {
        uint32_t rt_c = ctx->gpu_msaa_color_res ? ctx->gpu_msaa_color_res : ctx->gpu_color_res;
        uint32_t rt_d = ctx->gpu_msaa_depth_res ? ctx->gpu_msaa_depth_res : ctx->gpu_depth_res;
        ipo_gpu_cmd_set_target(&ctx->gpu_batch, rt_c, rt_d, (uint32_t)ctx->width, (uint32_t)ctx->height);
        int top_vp_y = ctx->height - (ctx->vp_y + ctx->vp_h);
        ipo_gpu_cmd_set_viewport(&ctx->gpu_batch, (float)ctx->vp_x, (float)top_vp_y,
                                 (float)ctx->vp_w, (float)ctx->vp_h,
                                 (float)ctx->depth_near, (float)ctx->depth_far);
    }
    return true;
}

bool gl_gpu_try_swap_buffers(GLContext *ctx, void *win_ptr) {
    if (!ctx || !ctx->gpu_frame_active) return false;
    wm_window_t *win = (wm_window_t *)(win_ptr ? win_ptr : ctx->win);

    uint32_t out_res = gl_gpu_resolve_output(ctx);
    if (win) {
        ipo_gpu_cmd_present_window(&ctx->gpu_batch, out_res, win,
                                   (uint32_t)ctx->width, (uint32_t)ctx->height);
    } else {
        ipo_gpu_cmd_present_screen(&ctx->gpu_batch, out_res,
                                   (uint32_t)ctx->width, (uint32_t)ctx->height);
    }

    gl_gpu_patch_and_flush(ctx);
    ctx->gpu_frame_active = false;
    ctx->gpu_dirty_content = false;
    return true;
}
