#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <driver/vbe_bga.h>
#include <ipo/gfx.h>
#include <wm.h>
#include <vga_gfx.h>
#include <memory/kmalloc.h>
#include <string.h>
#include <stdio.h>
#include <syscall.h>

static GLContext *g_current_context = NULL;

GLContext *gl_create_context(int width, int height, void *win) {
    if (width <= 0)  width  = 100;
    if (height <= 0) height = 60;

    GLContext *ctx = kmalloc(sizeof(GLContext));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(GLContext));

    ctx->width  = width;
    ctx->height = height;
    ctx->win    = win;

    /* Allocate 32-bit ARGB color buffer, 16-bit depth buffer, and 8-bit stencil buffer */
    size_t num_pixels = (size_t)width * (size_t)height;
    ctx->color_buffer   = kmalloc(num_pixels * sizeof(uint32_t));
    ctx->z_buffer       = kmalloc(num_pixels * sizeof(uint16_t));
    ctx->stencil_buffer = kmalloc(num_pixels * sizeof(uint8_t));

    if (!ctx->color_buffer || !ctx->z_buffer || !ctx->stencil_buffer) {
        gl_destroy_context(ctx);
        return NULL;
    }

    /* Initialize matrix stacks without hardcoded limits */
    gl_matrix_init_stack(&ctx->modelview_stack);
    gl_matrix_init_stack(&ctx->projection_stack);
    gl_matrix_init_stack(&ctx->texture_stack);
    ctx->matrix_mode = GL_MODELVIEW;

    /* Viewport */
    ctx->vp_x = 0;
    ctx->vp_y = 0;
    ctx->vp_w = width;
    ctx->vp_h = height;
    ctx->depth_near = 0.0;
    ctx->depth_far  = 1.0;

    /* Scissor */
    ctx->scissor_x = 0;
    ctx->scissor_y = 0;
    ctx->scissor_w = width;
    ctx->scissor_h = height;
    ctx->scissor_enabled = GL_FALSE;

    /* Clear values */
    ctx->clear_color[0] = 0.0f;
    ctx->clear_color[1] = 0.0f;
    ctx->clear_color[2] = 0.0f;
    ctx->clear_color[3] = 1.0f;
    ctx->clear_depth    = 1.0;
    ctx->clear_stencil  = 0;

    /* Depth Test */
    ctx->depth_test_enabled = GL_FALSE;
    ctx->depth_func         = GL_LESS;
    ctx->depth_mask         = GL_TRUE;

    /* Alpha Test */
    ctx->alpha_test_enabled = GL_FALSE;
    ctx->alpha_func         = GL_ALWAYS;
    ctx->alpha_ref          = 0.0f;

    /* Stencil Test */
    ctx->stencil_test_enabled = GL_FALSE;
    ctx->stencil_func         = GL_ALWAYS;
    ctx->stencil_ref          = 0;
    ctx->stencil_mask         = 0xFFFFFFFFu;
    ctx->stencil_fail         = GL_KEEP;
    ctx->stencil_zfail        = GL_KEEP;
    ctx->stencil_zpass        = GL_KEEP;
    ctx->stencil_writemask    = 0xFFFFFFFFu;

    /* Blending & LogicOp */
    ctx->blend_enabled   = GL_FALSE;
    ctx->blend_sfactor   = GL_ONE;
    ctx->blend_dfactor   = GL_ZERO;
    ctx->blend_equation  = GL_FUNC_ADD;
    ctx->logic_op_enabled = GL_FALSE;
    ctx->logic_opcode    = GL_COPY;

    /* Fog */
    ctx->fog_enabled = GL_FALSE;
    ctx->fog_mode    = GL_EXP;
    ctx->fog_density = 1.0f;
    ctx->fog_start   = 0.0f;
    ctx->fog_end     = 1.0f;
    ctx->fog_color[0] = 0.0f; ctx->fog_color[1] = 0.0f;
    ctx->fog_color[2] = 0.0f; ctx->fog_color[3] = 0.0f;

    /* Lighting & Materials */
    ctx->lighting_enabled = GL_FALSE;
    ctx->shade_model      = GL_SMOOTH;
    ctx->front_material.ambient[0] = 0.2f; ctx->front_material.ambient[1] = 0.2f;
    ctx->front_material.ambient[2] = 0.2f; ctx->front_material.ambient[3] = 1.0f;
    ctx->front_material.diffuse[0] = 0.8f; ctx->front_material.diffuse[1] = 0.8f;
    ctx->front_material.diffuse[2] = 0.8f; ctx->front_material.diffuse[3] = 1.0f;
    ctx->front_material.specular[0] = 0.0f; ctx->front_material.specular[1] = 0.0f;
    ctx->front_material.specular[2] = 0.0f; ctx->front_material.specular[3] = 1.0f;
    ctx->front_material.shininess = 0.0f;

    /* Light 0 default position (dynamically expandable light array) */
    ctx->light_capacity = 8;
    ctx->lights = kmalloc((size_t)ctx->light_capacity * sizeof(gl_light_t));
    if (!ctx->lights) {
        gl_destroy_context(ctx);
        return NULL;
    }
    memset(ctx->lights, 0, (size_t)ctx->light_capacity * sizeof(gl_light_t));
    ctx->lights[0].diffuse[0] = 1.0f; ctx->lights[0].diffuse[1] = 1.0f;
    ctx->lights[0].diffuse[2] = 1.0f; ctx->lights[0].diffuse[3] = 1.0f;
    ctx->lights[0].position[2] = 1.0f;

    /* Polygon mode */
    ctx->polygon_mode      = GL_FILL;
    ctx->cull_face_enabled = GL_FALSE;
    ctx->cull_face_mode    = GL_BACK;
    ctx->front_face        = GL_CCW;
    ctx->point_size        = 1.0f;
    ctx->line_width        = 1.0f;

    /* Current attributes */
    ctx->cur_color[0] = 1.0f; ctx->cur_color[1] = 1.0f;
    ctx->cur_color[2] = 1.0f; ctx->cur_color[3] = 1.0f;
    ctx->cur_normal[2] = 1.0f;
    ctx->cur_texcoord[0] = 0.0f; ctx->cur_texcoord[1] = 0.0f;
    ctx->cur_texcoord[2] = 0.0f; ctx->cur_texcoord[3] = 1.0f;

    /* Textures */
    ctx->tex_env_mode = GL_MODULATE;

    /* Display lists */
    ctx->list_capacity = 16;
    ctx->display_lists = kmalloc((size_t)ctx->list_capacity * sizeof(gl_display_list_t *));

    /* Vertex buffer for immediate mode */
    ctx->vert_buf.capacity = 64;
    ctx->vert_buf.count = 0;
    ctx->vert_buf.data = kmalloc((size_t)ctx->vert_buf.capacity * sizeof(gl_vertex_t));

    ctx->error_flag = GL_NO_ERROR;

    if (!g_current_context) {
        g_current_context = ctx;
    }

    return ctx;
}

void gl_destroy_context(GLContext *ctx) {
    if (!ctx) return;

    if (g_current_context == ctx) {
        g_current_context = NULL;
    }

    if (ctx->color_buffer)   kfree(ctx->color_buffer);
    if (ctx->z_buffer)       kfree(ctx->z_buffer);
    if (ctx->stencil_buffer) kfree(ctx->stencil_buffer);
    if (ctx->mode13h_frame)  kfree(ctx->mode13h_frame);

    gl_matrix_free_stack(&ctx->modelview_stack);
    gl_matrix_free_stack(&ctx->projection_stack);
    gl_matrix_free_stack(&ctx->texture_stack);

    if (ctx->vert_buf.data) kfree(ctx->vert_buf.data);
    if (ctx->clip_buf) kfree(ctx->clip_buf);

    if (ctx->textures) {
        for (int i = 0; i < ctx->texture_count; i++) {
            if (ctx->textures[i]) {
                gl_gpu_destroy_texture(NULL, ctx->textures[i]);
                if (ctx->textures[i]->pixels) kfree(ctx->textures[i]->pixels);
                kfree(ctx->textures[i]);
            }
        }
        kfree(ctx->textures);
    }
    gl_gpu_destroy_context_res(ctx);

    if (ctx->display_lists) {
        for (int i = 0; i < ctx->list_count; i++) {
            if (ctx->display_lists[i]) {
                if (ctx->display_lists[i]->commands) kfree(ctx->display_lists[i]->commands);
                kfree(ctx->display_lists[i]);
            }
        }
        kfree(ctx->display_lists);
    }

    if (ctx->lights) kfree(ctx->lights);
    kfree(ctx);
}

bool gl_ensure_light_capacity(GLContext *ctx, int min_count) {
    if (!ctx || min_count <= 0) return false;
    if (ctx->lights && min_count <= ctx->light_capacity) return true;
    int new_cap = ctx->light_capacity > 0 ? ctx->light_capacity : 8;
    while (new_cap < min_count) {
        if (new_cap > INT32_MAX / 2) { new_cap = min_count; break; }
        new_cap *= 2;
    }
    if ((size_t)new_cap > SIZE_MAX / sizeof(gl_light_t)) return false;
    gl_light_t *nl = (gl_light_t *)krealloc(ctx->lights, (size_t)new_cap * sizeof(gl_light_t));
    if (!nl) return false;
    memset(&nl[ctx->light_capacity], 0, (size_t)(new_cap - ctx->light_capacity) * sizeof(gl_light_t));
    ctx->lights = nl;
    ctx->light_capacity = new_cap;
    return true;
}

bool gl_resize_context(GLContext *ctx, int new_w, int new_h) {
    if (!ctx || new_w <= 0 || new_h <= 0) return false;
    if (ctx->width == new_w && ctx->height == new_h) return true;

    size_t npixels = (size_t)new_w * (size_t)new_h;
    uint32_t *new_color = kmalloc(npixels * sizeof(uint32_t));
    uint16_t *new_z = kmalloc(npixels * sizeof(uint16_t));
    uint8_t *new_stencil = kmalloc(npixels * sizeof(uint8_t));

    if (!new_color || !new_z || !new_stencil) {
        if (new_color) kfree(new_color);
        if (new_z) kfree(new_z);
        if (new_stencil) kfree(new_stencil);
        return false;
    }

    if (ctx->color_buffer) kfree(ctx->color_buffer);
    if (ctx->z_buffer) kfree(ctx->z_buffer);
    if (ctx->stencil_buffer) kfree(ctx->stencil_buffer);

    ctx->color_buffer = new_color;
    ctx->z_buffer = new_z;
    ctx->stencil_buffer = new_stencil;
    if (ctx->scissor_x == 0 && ctx->scissor_y == 0 &&
        ctx->scissor_w == ctx->width && ctx->scissor_h == ctx->height) {
        ctx->scissor_w = new_w;
        ctx->scissor_h = new_h;
    }
    ctx->width = new_w;
    ctx->height = new_h;
    ctx->vp_w = new_w;
    ctx->vp_h = new_h;
    gl_gpu_invalidate_rt(ctx);

    return true;
}

void gl_make_current(GLContext *ctx) {
    g_current_context = ctx;
}

GLContext *gl_get_current_context(void) {
    return g_current_context;
}

void gl_swap_buffers(GLContext *ctx, void *win_ptr) {
    if (!ctx) ctx = g_current_context;
    if (!ctx || !ctx->color_buffer) return;

    if (gl_gpu_try_swap_buffers(ctx, win_ptr)) return;

    wm_window_t *win = (wm_window_t *)(win_ptr ? win_ptr : ctx->win);

    int hw_bpp = 8;
    ipo_gfx_caps_t caps;
    if (ipo_gfx_get_caps(&caps) == 0 && caps.active_hw_bpp > 0u) {
        hw_bpp = (int)caps.active_hw_bpp;
    }

    if (hw_bpp >= 15) {
        ipo_gfx_surface_desc_t surf;
        memset(&surf, 0, sizeof(surf));
        surf.struct_size = sizeof(surf);
        surf.width = (uint32_t)ctx->width;
        surf.height = (uint32_t)ctx->height;
        surf.stride_bytes = (uint32_t)ctx->width * (uint32_t)sizeof(uint32_t);
        surf.format_id = IPO_COLOR_FMT_XRGB8888;
        surf.tonemap_op = IPO_TONEMAP_NONE;
        surf.exposure = 1.0;
        surf.pixels = ctx->color_buffer;
        if (win != NULL && wm_is_window_valid(win)) {
            if (ipo_gfx_present_window(win, &surf) == 0) return;
        } else {
            if (ipo_gfx_present_screen(&surf) == 0) return;
        }
    }

    if (win != NULL && win->framebuf != NULL && wm_is_window_valid(win)) {
        /*
         * Legacy 8bpp Mode 13h fallback:
         * Strided quantization so window resizing or dimensions never shear or misalign scanlines.
         */
        int w = ctx->width  < (int)win->w ? ctx->width  : (int)win->w;
        int h = ctx->height < (int)win->h ? ctx->height : (int)win->h;
        gl_quantize_argb_to_palette_strided(ctx->color_buffer, ctx->width, win->framebuf, win->w, w, h);
        wm_invalidate(win);
    } else {
        /*
         * Standalone 8bpp graphics: scale through kernel vga_gfx_flip_scaled_buffer
         */
        int w = ctx->width;
        int h = ctx->height;
        if (w > 0 && h > 0 && (size_t)w <= SIZE_MAX / (size_t)h) {
            size_t need = (size_t)w * (size_t)h;
            if (need > ctx->mode13h_frame_cap) {
                uint8_t *nf = (uint8_t *)krealloc(ctx->mode13h_frame, need);
                if (!nf) return;
                ctx->mode13h_frame = nf;
                ctx->mode13h_frame_cap = need;
            }
            gl_quantize_argb_to_palette_strided(ctx->color_buffer, ctx->width, ctx->mode13h_frame, w, w, h);
            vga_gfx_flip_scaled_buffer(ctx->mode13h_frame, w, h);
        }
    }
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->clear_color[0] = red;
    ctx->clear_color[1] = green;
    ctx->clear_color[2] = blue;
    ctx->clear_color[3] = alpha;
}

void glClearDepth(GLclampd depth) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->clear_depth = depth;
}

void glClearStencil(GLint s) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->clear_stencil = s;
}

void glClear(GLbitfield mask) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || ctx->width <= 0 || ctx->height <= 0) return;

    if (gl_gpu_try_clear(ctx, mask)) {
        return;
    }

    int cx0 = 0, cy0 = 0, cx1 = ctx->width, cy1 = ctx->height;
    bool use_scissor = ctx->scissor_enabled &&
        !(ctx->scissor_x == 0 && ctx->scissor_y == 0 &&
          ctx->scissor_w >= ctx->width && ctx->scissor_h >= ctx->height);
    if (use_scissor) {
        cx0 = ctx->scissor_x;
        cy0 = ctx->height - (ctx->scissor_y + ctx->scissor_h);
        cx1 = cx0 + ctx->scissor_w;
        cy1 = cy0 + ctx->scissor_h;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 > ctx->width) cx1 = ctx->width;
        if (cy1 > ctx->height) cy1 = ctx->height;
        if (cx1 <= cx0 || cy1 <= cy0) return;
    }

    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t r = (uint32_t)(ctx->clear_color[0] * 255.0f);
        uint32_t g = (uint32_t)(ctx->clear_color[1] * 255.0f);
        uint32_t b = (uint32_t)(ctx->clear_color[2] * 255.0f);
        uint32_t a = (uint32_t)(ctx->clear_color[3] * 255.0f);
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        if (a > 255) a = 255;
        uint32_t pixel = (a << 24) | (r << 16) | (g << 8) | b;

        for (int y = cy0; y < cy1; y++) {
            uint32_t *row = ctx->color_buffer + (size_t)y * (size_t)ctx->width;
            for (int x = cx0; x < cx1; x++) {
                row[x] = pixel;
            }
        }
    }

    if (mask & GL_DEPTH_BUFFER_BIT) {
        uint16_t zval = (uint16_t)(ctx->clear_depth * 65535.0);
        for (int y = cy0; y < cy1; y++) {
            uint16_t *row = ctx->z_buffer + (size_t)y * (size_t)ctx->width;
            for (int x = cx0; x < cx1; x++) {
                row[x] = zval;
            }
        }
    }

    if (mask & GL_STENCIL_BUFFER_BIT) {
        uint8_t sval = (uint8_t)(ctx->clear_stencil & 0xFF);
        for (int y = cy0; y < cy1; y++) {
            uint8_t *row = ctx->stencil_buffer + (size_t)y * (size_t)ctx->width;
            memset(row + cx0, sval, (size_t)(cx1 - cx0));
        }
    }
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    if (x == 0 && y == 0 && width > 0 && height > 0 &&
        (ctx->width != width || ctx->height != height)) {
        gl_resize_context(ctx, width, height);
    }
    ctx->vp_x = x;
    ctx->vp_y = y;
    ctx->vp_w = width;
    ctx->vp_h = height;
}

void glDepthRange(GLclampd nearVal, GLclampd farVal) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->depth_near = nearVal;
    ctx->depth_far  = farVal;
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->scissor_x = x;
    ctx->scissor_y = y;
    ctx->scissor_w = width;
    ctx->scissor_h = height;
}

void glEnable(GLenum cap) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    switch (cap) {
        case GL_DEPTH_TEST:     ctx->depth_test_enabled   = GL_TRUE; break;
        case GL_ALPHA_TEST:     ctx->alpha_test_enabled   = GL_TRUE; break;
        case GL_STENCIL_TEST:   ctx->stencil_test_enabled = GL_TRUE; break;
        case GL_BLEND:          ctx->blend_enabled        = GL_TRUE; break;
        case GL_COLOR_LOGIC_OP: ctx->logic_op_enabled     = GL_TRUE; break;
        case GL_SCISSOR_TEST:   ctx->scissor_enabled      = GL_TRUE; break;
        case GL_CULL_FACE:      ctx->cull_face_enabled    = GL_TRUE; break;
        case GL_LIGHTING:       ctx->lighting_enabled     = GL_TRUE; break;
        case GL_FOG:            ctx->fog_enabled          = GL_TRUE; break;
        case GL_TEXTURE_2D:     ctx->texture_2d_enabled   = GL_TRUE; break;
        case GL_TEXTURE_3D:     ctx->texture_3d_enabled   = GL_TRUE; break;
        case GL_COLOR_MATERIAL: ctx->color_material_enabled = GL_TRUE; break;
        case GL_NORMALIZE:      ctx->normalize_normals    = GL_TRUE; break;
        default:
            if (cap >= GL_LIGHT0 && (cap - GL_LIGHT0) <= (GLenum)INT32_MAX - 1u) {
                int idx = (int)(cap - GL_LIGHT0);
                if (gl_ensure_light_capacity(ctx, idx + 1)) {
                    ctx->lights[idx].enabled = GL_TRUE;
                } else {
                    ctx->error_flag = GL_OUT_OF_MEMORY;
                }
            } else {
                ctx->error_flag = GL_INVALID_ENUM;
            }
            break;
    }
}

void glDisable(GLenum cap) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    switch (cap) {
        case GL_DEPTH_TEST:     ctx->depth_test_enabled   = GL_FALSE; break;
        case GL_ALPHA_TEST:     ctx->alpha_test_enabled   = GL_FALSE; break;
        case GL_STENCIL_TEST:   ctx->stencil_test_enabled = GL_FALSE; break;
        case GL_BLEND:          ctx->blend_enabled        = GL_FALSE; break;
        case GL_COLOR_LOGIC_OP: ctx->logic_op_enabled     = GL_FALSE; break;
        case GL_SCISSOR_TEST:   ctx->scissor_enabled      = GL_FALSE; break;
        case GL_CULL_FACE:      ctx->cull_face_enabled    = GL_FALSE; break;
        case GL_LIGHTING:       ctx->lighting_enabled     = GL_FALSE; break;
        case GL_FOG:            ctx->fog_enabled          = GL_FALSE; break;
        case GL_TEXTURE_2D:     ctx->texture_2d_enabled   = GL_FALSE; break;
        case GL_TEXTURE_3D:     ctx->texture_3d_enabled   = GL_FALSE; break;
        case GL_COLOR_MATERIAL: ctx->color_material_enabled = GL_FALSE; break;
        case GL_NORMALIZE:      ctx->normalize_normals    = GL_FALSE; break;
        default:
            if (cap >= GL_LIGHT0 && (cap - GL_LIGHT0) <= (GLenum)INT32_MAX - 1u) {
                int idx = (int)(cap - GL_LIGHT0);
                if (idx < ctx->light_capacity && ctx->lights) {
                    ctx->lights[idx].enabled = GL_FALSE;
                }
            } else {
                ctx->error_flag = GL_INVALID_ENUM;
            }
            break;
    }
}

GLboolean glIsEnabled(GLenum cap) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return GL_FALSE;
    switch (cap) {
        case GL_DEPTH_TEST:     return ctx->depth_test_enabled;
        case GL_ALPHA_TEST:     return ctx->alpha_test_enabled;
        case GL_STENCIL_TEST:   return ctx->stencil_test_enabled;
        case GL_BLEND:          return ctx->blend_enabled;
        case GL_COLOR_LOGIC_OP: return ctx->logic_op_enabled;
        case GL_SCISSOR_TEST:   return ctx->scissor_enabled;
        case GL_CULL_FACE:      return ctx->cull_face_enabled;
        case GL_LIGHTING:       return ctx->lighting_enabled;
        case GL_FOG:            return ctx->fog_enabled;
        case GL_TEXTURE_2D:     return ctx->texture_2d_enabled;
        case GL_TEXTURE_3D:     return ctx->texture_3d_enabled;
        default:
            if (cap >= GL_LIGHT0) {
                int idx = (int)(cap - GL_LIGHT0);
                if (idx >= 0 && idx < ctx->light_capacity && ctx->lights) {
                    return ctx->lights[idx].enabled;
                }
            }
            return GL_FALSE;
    }
}
