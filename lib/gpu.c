/* Userspace VirtIO-GPU / VirGL client helpers and dynamic batch builder. */

#include <ipo/gpu_client.h>
#include <syscall.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

bool ipo_gpu_get_info(ipo_gpu_info_t *out_info) {
    if (!out_info) return false;
    static char *s_gpu_name = NULL;
    static uint32_t s_gpu_name_cap = 0u;
    if (!s_gpu_name || s_gpu_name_cap < 64u) {
        char *nb = (char *)realloc(s_gpu_name, 64u);
        if (nb) {
            s_gpu_name = nb;
            s_gpu_name_cap = 64u;
        }
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->name = s_gpu_name;
    out_info->name_cap = s_gpu_name_cap;
    if (ipo_sys_gpu_get_info(out_info) != 0) return false;
    if (out_info->name_len + 1u > s_gpu_name_cap) {
        uint32_t ncap = out_info->name_len + 1u;
        char *nb = (char *)realloc(s_gpu_name, ncap);
        if (nb) {
            s_gpu_name = nb;
            s_gpu_name_cap = ncap;
            out_info->name = s_gpu_name;
            out_info->name_cap = s_gpu_name_cap;
            (void)ipo_sys_gpu_get_info(out_info);
        }
    }
    return true;
}

int ipo_gpu_set_mode(uint32_t force_cpu) {
    return ipo_sys_gpu_set_mode(force_cpu);
}

bool ipo_gpu_3d_available(void) {
    ipo_gpu_info_t info;
    if (!ipo_gpu_get_info(&info)) return false;
    return info.hw_detected && info.virgl_3d_supported && !info.forced_cpu_fallback &&
           info.backend_mode == IPO_GPU_BACKEND_VIRGL_3D;
}

bool ipo_gpu_2d_available(void) {
    ipo_gpu_info_t info;
    if (!ipo_gpu_get_info(&info)) return false;
    return info.hw_detected && !info.forced_cpu_fallback &&
           info.backend_mode >= IPO_GPU_BACKEND_VIRTIO_2D;
}

uint32_t ipo_gpu_max_samples(void) {
    ipo_gpu_info_t info;
    if (!ipo_gpu_get_info(&info)) return 0u;
    if (!info.hw_detected || !info.virgl_3d_supported || info.forced_cpu_fallback) return 0u;
    return (uint32_t)info.reserved0;
}

static void ipo_gpu_eval_studio_env(float rx, float ry, float rz, float blur_spread, float out[4]) {
    float r = 0.025f, g = 0.028f, b = 0.038f;
    if (ry >= 0.0f) {
        float t = 1.0f - ry;
        float dome = 0.042f + 0.18f * t * t;
        r += dome * 0.85f;
        g += dome * 0.92f;
        b += dome * 1.12f;
    } else {
        float ring = 1.0f + ry;
        if (ring < 0.0f) ring = 0.0f;
        float flr = 0.025f + 0.15f * ring * ring;
        r += flr * 1.06f;
        g += flr * 0.96f;
        b += flr * 0.84f;
    }

    float sp = 1.0f + blur_spread * 1.4f;
    float abs_ry = (ry >= 0.0f) ? ry : -ry;
    float h_band = 1.0f - (abs_ry * (4.8f / sp) > 1.0f ? 1.0f : abs_ry * (4.8f / sp));
    float h_glow = h_band * h_band * (0.34f / sp);
    r += h_glow * 0.98f;
    g += h_glow * 0.99f;
    b += h_glow * 1.05f;

    /* 1. Large curved key overhead-right octabox with luminous grid contrast & hot core */
    {
        float dx = (rx - 0.50f) / (0.30f * sp);
        float dy = (ry - 0.54f) / (0.27f * sp);
        float dz = (rz - 0.36f) / (0.36f * sp);
        float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 < 1.18f) {
            float t = 1.0f - dist2 / 1.18f;
            float soft = t * t * (3.0f - 2.0f * t);
            float core = (dist2 < 0.28f) ? (1.35f + 0.70f * (1.0f - dist2 / 0.28f)) : 1.0f;
            /* Subtle softbox window diffuser frame at mid spread when sharp */
            float muntin = 1.0f;
            if (blur_spread < 0.45f) {
                float mx = fabsf(dx);
                float my = fabsf(dy);
                if (mx < 0.045f || my < 0.045f) muntin = 0.78f;
            }
            float att = (4.20f * core * muntin) / (1.0f + blur_spread * 0.42f);
            r += soft * att * 1.00f;
            g += soft * att * 0.94f;
            b += soft * att * 0.83f;
        }
    }

    /* 2. Crisp cool vertical studio strip softbox on upper-left */
    {
        float dx = (rx + 0.66f) / (0.16f * sp);
        float dy = (ry - 0.28f) / (0.46f * sp);
        float dz = (rz - 0.08f) / (0.38f * sp);
        float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 < 1.08f) {
            float t = 1.0f - dist2 / 1.08f;
            float soft = t * t * (3.0f - 2.0f * t);
            float att = 2.45f / (1.0f + blur_spread * 0.38f);
            r += soft * att * 0.68f;
            g += soft * att * 0.89f;
            b += soft * att * 1.24f;
        }
    }

    /* 3. Overhead cyclorama studio rim softbox + right edge kicker arc */
    {
        float dx = (rx - 0.02f) / (0.44f * sp);
        float dy = (ry - 0.74f) / (0.18f * sp);
        float dz = (rz + 0.58f) / (0.30f * sp);
        float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 < 1.10f) {
            float t = 1.0f - dist2 / 1.10f;
            float soft = t * t;
            float att = 2.35f / (1.0f + blur_spread * 0.38f);
            r += soft * att * 0.96f;
            g += soft * att * 0.98f;
            b += soft * att * 1.08f;
        }
    }
    {
        float dx = (rx - 0.78f) / (0.15f * sp);
        float dy = (ry - 0.14f) / (0.32f * sp);
        float dz = (rz + 0.48f) / (0.28f * sp);
        float dist2 = dx * dx + dy * dy + dz * dz;
        if (dist2 < 1.0f) {
            float t = 1.0f - dist2;
            float att = (1.65f * t * t) / (1.0f + blur_spread * 0.45f);
            r += att * 1.05f;
            g += att * 0.76f;
            b += att * 0.52f;
        }
    }

    /* 4. Negative-fill dark studio velvet baffles for dramatic liquid-metal & glaze contrast */
    if (rz > 0.34f && blur_spread < 0.8f) {
        float dx = (rx - 0.01f) / 0.32f;
        float dy = (ry + 0.03f) / 0.40f;
        float d2 = dx * dx + dy * dy;
        if (d2 < 1.0f) {
            float t = 1.0f - d2;
            float fade = 1.0f - 0.72f * t * t;
            r *= fade;
            g *= fade;
            b *= fade;
        }
    }

    out[0] = r; out[1] = g; out[2] = b; out[3] = 1.0f;
}

static uint32_t ipo_gpu_bake_octahedral_hdri(uint32_t ctx_id, int sz, float blur_spread) {
    float *pix = (float *)malloc((size_t)sz * (size_t)sz * 4u * sizeof(float));
    if (!pix) return 0u;
    for (int y = 0; y < sz; y++) {
        float oz_in = ((float)y + 0.5f) / (float)sz * 2.0f - 1.0f;
        for (int x = 0; x < sz; x++) {
            float ox_in = ((float)x + 0.5f) / (float)sz * 2.0f - 1.0f;
            float ax = ox_in >= 0.0f ? ox_in : -ox_in;
            float az = oz_in >= 0.0f ? oz_in : -oz_in;
            float oy = 1.0f - ax - az;
            float ox = ox_in, oz = oz_in;
            if (oy < 0.0f) {
                ox = (1.0f - az) * (ox_in >= 0.0f ? 1.0f : -1.0f);
                oz = (1.0f - ax) * (oz_in >= 0.0f ? 1.0f : -1.0f);
            }
            float l = sqrtf(ox * ox + oy * oy + oz * oz);
            if (l < 1e-6f) l = 1.0f;
            float *dst = &pix[((size_t)y * (size_t)sz + (size_t)x) * 4u];
            ipo_gpu_eval_studio_env(ox / l, oy / l, oz / l, blur_spread, dst);
        }
    }
    uint32_t rid = ipo_gpu_res_create_tex2d(ctx_id, IPO_GPU_FMT_RGBA32F, (uint32_t)sz, (uint32_t)sz,
                                            pix, (uint32_t)sz * 16u);
    free(pix);
    return rid;
}

bool ipo_gpu_create_studio_env(uint32_t ctx_id, uint32_t *out_sharp_res, uint32_t *out_soft_res) {
    if (!ctx_id || !out_sharp_res || !out_soft_res) return false;
    uint32_t sharp = ipo_gpu_bake_octahedral_hdri(ctx_id, 256, 0.0f);
    uint32_t soft  = ipo_gpu_bake_octahedral_hdri(ctx_id, 64, 1.35f);
    if (!sharp || !soft) {
        if (sharp) ipo_gpu_res_destroy(sharp);
        if (soft) ipo_gpu_res_destroy(soft);
        return false;
    }
    *out_sharp_res = sharp;
    *out_soft_res = soft;
    return true;
}

uint32_t ipo_gpu_ctx_create(const char *debug_name) {
    uint32_t cid = 0u;
    if (ipo_sys_gpu_ctx_create(&cid, debug_name) != 0) return 0u;
    return cid;
}

int ipo_gpu_ctx_destroy(uint32_t ctx_id) {
    if (!ctx_id) return -1;
    return ipo_sys_gpu_ctx_destroy(ctx_id);
}

uint32_t ipo_gpu_res_create(uint32_t ctx_id, const ipo_gpu_res_desc_t *desc) {
    if (!desc) return 0u;
    uint32_t rid = 0u;
    if (ipo_sys_gpu_res_create(ctx_id, desc, &rid) != 0) return 0u;
    return rid;
}

int ipo_gpu_res_upload_buf(uint32_t ctx_id, uint32_t res_id, uint32_t offset_bytes,
                           const void *data, uint32_t byte_size) {
    if (!res_id || !data || byte_size == 0u) return -1;
    ipo_gpu_upload_desc_t u;
    memset(&u, 0, sizeof(u));
    u.struct_size  = sizeof(u);
    u.ctx_id       = ctx_id;
    u.res_id       = res_id;
    u.offset_bytes = offset_bytes;
    u.byte_size    = byte_size;
    u.data         = data;
    return ipo_sys_gpu_res_upload(&u);
}

int ipo_gpu_res_upload_tex2d(uint32_t ctx_id, uint32_t res_id,
                             uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                             uint32_t stride_bytes, const void *pixels) {
    if (!res_id || !pixels || w == 0u || h == 0u) return -1;
    uint32_t sb = stride_bytes ? stride_bytes : (w * 4u);
    uint32_t bsz = sb * h;
    ipo_gpu_upload_desc_t u;
    memset(&u, 0, sizeof(u));
    u.struct_size  = sizeof(u);
    u.ctx_id       = ctx_id;
    u.res_id       = res_id;
    u.x = x; u.y = y; u.w = w; u.h = h;
    u.stride_bytes = sb;
    u.byte_size    = bsz;
    u.data         = pixels;
    return ipo_sys_gpu_res_upload(&u);
}

uint32_t ipo_gpu_res_create_buf(uint32_t ctx_id, uint32_t kind, const void *init_data, uint32_t byte_size) {
    if (byte_size == 0u) return 0u;
    ipo_gpu_res_desc_t d;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    d.kind        = kind;
    d.format      = IPO_GPU_FMT_RAW;
    d.byte_size   = byte_size;
    uint32_t rid = ipo_gpu_res_create(ctx_id, &d);
    if (rid && init_data) {
        if (ipo_gpu_res_upload_buf(ctx_id, rid, 0u, init_data, byte_size) != 0) {
            ipo_gpu_res_destroy(rid);
            return 0u;
        }
    }
    return rid;
}

uint32_t ipo_gpu_res_create_tex2d(uint32_t ctx_id, uint32_t format, uint32_t w, uint32_t h,
                                  const void *pixels, uint32_t stride_bytes) {
    if (w == 0u || h == 0u) return 0u;
    ipo_gpu_res_desc_t d;
    memset(&d, 0, sizeof(d));
    d.struct_size  = sizeof(d);
    d.kind         = IPO_GPU_RES_TEX_2D;
    d.format       = format;
    d.flags        = IPO_GPU_RES_FLAG_SAMPLER;
    d.width        = w;
    d.height       = h;
    d.stride_bytes = stride_bytes;
    uint32_t rid = ipo_gpu_res_create(ctx_id, &d);
    if (rid && pixels) {
        if (ipo_gpu_res_upload_tex2d(ctx_id, rid, 0u, 0u, w, h, stride_bytes, pixels) != 0) {
            ipo_gpu_res_destroy(rid);
            return 0u;
        }
    }
    return rid;
}

uint32_t ipo_gpu_res_create_rt(uint32_t ctx_id, uint32_t format, uint32_t w, uint32_t h, uint32_t extra_flags) {
    if (w == 0u || h == 0u) return 0u;
    ipo_gpu_res_desc_t d;
    memset(&d, 0, sizeof(d));
    d.struct_size = sizeof(d);
    d.kind        = (format == IPO_GPU_FMT_Z32F) ? IPO_GPU_RES_DEPTH_TARGET : IPO_GPU_RES_COLOR_TARGET;
    d.format      = format;
    d.flags       = ((d.kind == IPO_GPU_RES_DEPTH_TARGET) ? IPO_GPU_RES_FLAG_DEPTH_STENCIL : IPO_GPU_RES_FLAG_RENDER_TARGET) | extra_flags;
    d.width       = w;
    d.height      = h;
    return ipo_gpu_res_create(ctx_id, &d);
}

int ipo_gpu_res_destroy(uint32_t res_id) {
    if (!res_id) return -1;
    return ipo_sys_gpu_res_destroy(res_id);
}

bool ipo_gpu_dynbuf_upload(uint32_t ctx_id, ipo_gpu_dynbuf_t *db, uint32_t kind,
                           const void *data, uint32_t byte_size) {
    if (!db || !data || byte_size == 0u) return false;
    if (db->res_id == 0u || byte_size > db->byte_cap) {
        if (db->res_id != 0u) {
            ipo_gpu_res_destroy(db->res_id);
            db->res_id = 0u;
        }
        uint32_t alloc_sz = db->byte_cap ? db->byte_cap : 65536u;
        while (alloc_sz < byte_size) {
            if (alloc_sz > 0x40000000u) { alloc_sz = byte_size; break; }
            alloc_sz *= 2u;
        }
        db->res_id = ipo_gpu_res_create_buf(ctx_id, kind, NULL, alloc_sz);
        if (!db->res_id) { db->byte_cap = 0u; return false; }
        db->byte_cap = alloc_sz;
    }
    return ipo_gpu_res_upload_buf(ctx_id, db->res_id, 0u, data, byte_size) == 0;
}

void ipo_gpu_dynbuf_destroy(ipo_gpu_dynbuf_t *db) {
    if (!db) return;
    if (db->res_id) {
        ipo_gpu_res_destroy(db->res_id);
        db->res_id = 0u;
    }
    db->byte_cap = 0u;
}

void ipo_gpu_batch_init(ipo_gpu_batch_t *b) {
    if (b) memset(b, 0, sizeof(*b));
}

void ipo_gpu_batch_reset(ipo_gpu_batch_t *b) {
    if (!b) return;
    b->cmd_count = 0u;
    b->const_used = 0u;
}

void ipo_gpu_batch_free(ipo_gpu_batch_t *b) {
    if (!b) return;
    if (b->cmds) free(b->cmds);
    if (b->const_blob) free(b->const_blob);
    memset(b, 0, sizeof(*b));
}

bool ipo_gpu_batch_push(ipo_gpu_batch_t *b, const ipo_gpu_cmd_t *cmd) {
    if (!b || !cmd) return false;
    if (b->cmd_count >= b->cmd_cap) {
        uint32_t ncap = b->cmd_cap ? (b->cmd_cap * 2u) : 64u;
        ipo_gpu_cmd_t *na = (ipo_gpu_cmd_t *)realloc(b->cmds, (size_t)ncap * sizeof(ipo_gpu_cmd_t));
        if (!na) return false;
        b->cmds = na;
        b->cmd_cap = ncap;
    }
    b->cmds[b->cmd_count++] = *cmd;
    return true;
}

bool ipo_gpu_cmd_set_target(ipo_gpu_batch_t *b, uint32_t color_res, uint32_t depth_res, uint32_t w, uint32_t h) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_SET_TARGET;
    c.u.target.color_res = color_res;
    c.u.target.depth_res = depth_res;
    c.u.target.width     = w;
    c.u.target.height    = h;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_clear(ipo_gpu_batch_t *b, uint32_t mask, const float rgba[4], float depth) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_CLEAR;
    c.u.clear.mask = mask;
    if (rgba) {
        c.u.clear.r = rgba[0]; c.u.clear.g = rgba[1];
        c.u.clear.b = rgba[2]; c.u.clear.a = rgba[3];
    }
    c.u.clear.depth = depth;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_set_viewport(ipo_gpu_batch_t *b, float x, float y, float w, float h, float znear, float zfar) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_SET_VIEWPORT;
    c.u.viewport.x = x; c.u.viewport.y = y; c.u.viewport.w = w; c.u.viewport.h = h;
    c.u.viewport.znear = znear; c.u.viewport.zfar = zfar;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_set_scissor(ipo_gpu_batch_t *b, bool enable, int32_t x, int32_t y, int32_t w, int32_t h) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_SET_SCISSOR;
    c.u.scissor.enable = enable ? 1u : 0u;
    c.u.scissor.x = x; c.u.scissor.y = y; c.u.scissor.w = w; c.u.scissor.h = h;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_set_pipeline_ex(ipo_gpu_batch_t *b, uint32_t program, uint8_t cull_mode, bool front_ccw,
                                 bool depth_test, bool depth_write, uint8_t depth_func,
                                 uint8_t blend_mode, uint8_t blend_eq,
                                 uint8_t blend_src, uint8_t blend_dst,
                                 bool wireframe, float point_size, float line_width) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_SET_PIPELINE;
    c.u.pipeline.program     = program;
    c.u.pipeline.cull_mode   = cull_mode;
    c.u.pipeline.front_ccw   = front_ccw ? 1u : 0u;
    c.u.pipeline.depth_test  = depth_test ? 1u : 0u;
    c.u.pipeline.depth_write = depth_write ? 1u : 0u;
    c.u.pipeline.depth_func  = depth_func;
    c.u.pipeline.blend_mode  = blend_mode;
    c.u.pipeline.blend_eq    = blend_eq;
    c.u.pipeline.blend_src   = blend_src;
    c.u.pipeline.blend_dst   = blend_dst;
    c.u.pipeline.wireframe   = wireframe ? 1u : 0u;
    c.u.pipeline.point_size  = point_size;
    c.u.pipeline.line_width  = line_width;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_set_pipeline(ipo_gpu_batch_t *b, uint32_t program, uint8_t cull_mode, bool front_ccw,
                              bool depth_test, bool depth_write, uint8_t depth_func,
                              uint8_t blend_mode, bool wireframe) {
    return ipo_gpu_cmd_set_pipeline_ex(b, program, cull_mode, front_ccw, depth_test, depth_write,
                                       depth_func, blend_mode, IPO_GPU_BEQ_ADD,
                                       IPO_GPU_BF_DEFAULT, IPO_GPU_BF_DEFAULT,
                                       wireframe, 1.0f, 1.0f);
}

bool ipo_gpu_cmd_set_constants(ipo_gpu_batch_t *b, uint32_t shader_stage, const float *floats, uint32_t float_count) {
    if (!b || !floats || float_count == 0u || float_count > (UINT32_MAX / 4u)) return false;
    if (b->const_used > UINT32_MAX - 15u) return false;
    uint32_t off = (b->const_used + 15u) & ~15u;
    uint32_t add = float_count * 4u;
    if (off > UINT32_MAX - add) return false;
    uint32_t need = off + add;
    if (need > b->const_cap) {
        uint32_t ncap = b->const_cap ? b->const_cap : 4096u;
        while (ncap < need) {
            if (ncap > UINT32_MAX / 2u) { ncap = need; break; }
            ncap *= 2u;
        }
        uint8_t *nb = (uint8_t *)realloc(b->const_blob, (size_t)ncap);
        if (!nb) return false;
        b->const_blob = nb;
        b->const_cap = ncap;
    }
    memcpy(b->const_blob + off, floats, add);
    b->const_used = need;

    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_SET_CONSTANTS;
    c.u.constants.shader_stage      = shader_stage;
    c.u.constants.blob_offset_bytes = off;
    c.u.constants.float_count       = float_count;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_bind_tex(ipo_gpu_batch_t *b, uint32_t slot, uint32_t tex_res, bool linear, bool wrap_repeat) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_BIND_TEXTURE;
    c.u.bind_tex.slot          = slot;
    c.u.bind_tex.tex_res       = tex_res;
    c.u.bind_tex.filter_linear = linear ? 1u : 0u;
    c.u.bind_tex.wrap_repeat   = wrap_repeat ? 1u : 0u;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_draw(ipo_gpu_batch_t *b, uint32_t vbo_res, uint32_t ibo_res, uint32_t index_size,
                      uint32_t prim_mode, uint32_t start, uint32_t count,
                      uint32_t vbo_offset_bytes, uint32_t ibo_offset_bytes) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_DRAW;
    c.u.draw.vbo_res          = vbo_res;
    c.u.draw.ibo_res          = ibo_res;
    c.u.draw.index_size       = index_size;
    c.u.draw.prim_mode        = prim_mode;
    c.u.draw.start            = start;
    c.u.draw.count            = count;
    c.u.draw.vbo_offset_bytes = vbo_offset_bytes;
    c.u.draw.ibo_offset_bytes = ibo_offset_bytes;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_blit(ipo_gpu_batch_t *b, uint32_t dst_res, uint32_t src_res,
                      int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                      int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                      bool linear, bool alpha_blend) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_BLIT;
    c.u.blit.dst_res       = dst_res;
    c.u.blit.src_res       = src_res;
    c.u.blit.dst_x = dx; c.u.blit.dst_y = dy; c.u.blit.dst_w = dw; c.u.blit.dst_h = dh;
    c.u.blit.src_x = sx; c.u.blit.src_y = sy; c.u.blit.src_w = sw; c.u.blit.src_h = sh;
    c.u.blit.filter_linear = linear ? 1u : 0u;
    c.u.blit.alpha_blend   = alpha_blend ? 1u : 0u;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_readback(ipo_gpu_batch_t *b, uint32_t res_id, uint32_t x, uint32_t y,
                          uint32_t w, uint32_t h, void *dst_ptr, uint32_t dst_stride_bytes) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_READBACK;
    c.u.readback.res_id           = res_id;
    c.u.readback.x = x; c.u.readback.y = y; c.u.readback.w = w; c.u.readback.h = h;
    c.u.readback.dst_user_ptr     = (uint64_t)(uintptr_t)dst_ptr;
    c.u.readback.dst_stride_bytes = dst_stride_bytes;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_present_window(ipo_gpu_batch_t *b, uint32_t res_id, wm_window_t *win,
                                uint32_t src_w, uint32_t src_h) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_PRESENT_WINDOW;
    c.u.present.res_id        = res_id;
    c.u.present.wm_window_ptr = (uint64_t)(uintptr_t)win;
    c.u.present.src_w         = src_w;
    c.u.present.src_h         = src_h;
    return ipo_gpu_batch_push(b, &c);
}

bool ipo_gpu_cmd_present_screen(ipo_gpu_batch_t *b, uint32_t res_id, uint32_t src_w, uint32_t src_h) {
    ipo_gpu_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.op = IPO_GPU_CMD_PRESENT_SCREEN;
    c.u.present.res_id = res_id;
    c.u.present.src_w  = src_w;
    c.u.present.src_h  = src_h;
    return ipo_gpu_batch_push(b, &c);
}

int ipo_gpu_batch_flush(ipo_gpu_batch_t *b, uint32_t ctx_id) {
    if (!b || !ctx_id) return -1;
    if (b->cmd_count == 0u) return 0;

    int rc = 0;
    ipo_gpu_batch_desc_t d;
    memset(&d, 0, sizeof(d));
    d.struct_size      = sizeof(d);
    d.ctx_id           = ctx_id;
    d.cmds             = b->cmds;
    d.cmd_count        = b->cmd_count;
    d.const_blob       = b->const_blob;
    d.const_blob_bytes = b->const_used;
    if (ipo_sys_gpu_submit_batch(&d) != 0) {
        rc = -1;
    }
    ipo_gpu_batch_reset(b);
    return rc;
}
