#include <GL/gl.h>
#include <GL/ipo_gl.h>
#include <memory/kmalloc.h>
#include <string.h>

GLuint glGenLists(GLsizei range) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || range <= 0) return 0;

    /* Reuse any contiguous block of deleted slots first */
    if (ctx->list_count >= range) {
        for (int start = 0; start <= ctx->list_count - range; start++) {
            bool all_free = true;
            for (GLsizei j = 0; j < range; j++) {
                gl_display_list_t *slot = ctx->display_lists[start + j];
                if (slot && slot->valid) {
                    all_free = false;
                    break;
                }
            }
            if (all_free) {
                for (GLsizei j = 0; j < range; j++) {
                    gl_display_list_t *dl = ctx->display_lists[start + j];
                    if (!dl) {
                        dl = kmalloc(sizeof(gl_display_list_t));
                        ctx->display_lists[start + j] = dl;
                    }
                    if (dl) {
                        memset(dl, 0, sizeof(gl_display_list_t));
                        dl->valid = true;
                    }
                }
                return (GLuint)(start + 1);
            }
        }
    }

    /* Ensure display list registry has space */
    while (ctx->list_count + range > ctx->list_capacity) {
        int new_cap = ctx->list_capacity ? ctx->list_capacity * 2 : 16;
        gl_display_list_t **new_arr = krealloc(ctx->display_lists, (size_t)new_cap * sizeof(gl_display_list_t *));
        if (!new_arr) return 0;
        ctx->display_lists = new_arr;
        ctx->list_capacity = new_cap;
    }

    GLuint base = (GLuint)(ctx->list_count + 1);
    for (GLsizei i = 0; i < range; i++) {
        gl_display_list_t *dl = kmalloc(sizeof(gl_display_list_t));
        if (dl) {
            memset(dl, 0, sizeof(gl_display_list_t));
            dl->valid = true;
            ctx->display_lists[ctx->list_count++] = dl;
        }
    }
    return base;
}

typedef struct {
    uint32_t op;
    GLenum   mode;
    GLfloat  v[4];
} gl_dl_cmd_t;

void gl_list_record_cmd(GLContext *ctx, uint32_t op, GLenum mode, const GLfloat *vals, int nvals) {
    if (!ctx || !ctx->is_compiling_list || ctx->current_list == 0 ||
        (int)ctx->current_list > ctx->list_count) {
        return;
    }
    gl_display_list_t *dl = ctx->display_lists[ctx->current_list - 1];
    if (!dl || !dl->valid) return;
    size_t need = dl->size + sizeof(gl_dl_cmd_t);
    if (need > dl->capacity) {
        size_t ncap = dl->capacity ? dl->capacity * 2u : 4096u;
        while (ncap < need) ncap *= 2u;
        uint8_t *nb = (uint8_t *)krealloc(dl->commands, ncap);
        if (!nb) return;
        dl->commands = nb;
        dl->capacity = ncap;
    }
    gl_dl_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = op;
    cmd.mode = mode;
    for (int i = 0; i < nvals && i < 4 && vals; i++) cmd.v[i] = vals[i];
    memcpy(dl->commands + dl->size, &cmd, sizeof(cmd));
    dl->size += sizeof(cmd);
}

void glNewList(GLuint list, GLenum mode) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || list == 0 || (int)list > ctx->list_count) return;

    if (mode != GL_COMPILE && mode != GL_COMPILE_AND_EXECUTE) {
        ctx->error_flag = GL_INVALID_ENUM;
        return;
    }

    gl_display_list_t *dl = ctx->display_lists[list - 1];
    if (dl) {
        dl->size = 0u;
        dl->valid = true;
    }
    ctx->current_list = list;
    ctx->list_compile_mode = mode;
    ctx->is_compiling_list = true;
}

void glEndList(void) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx) return;
    ctx->is_compiling_list = false;
    ctx->current_list = 0;
}

void glCallList(GLuint list) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || list == 0 || (int)list > ctx->list_count) return;

    gl_display_list_t *dl = ctx->display_lists[list - 1];
    if (!dl || !dl->valid || !dl->commands || dl->size < sizeof(gl_dl_cmd_t)) return;

    bool prev_comp = ctx->is_compiling_list;
    ctx->is_compiling_list = false;
    size_t count = dl->size / sizeof(gl_dl_cmd_t);
    const gl_dl_cmd_t *cmds = (const gl_dl_cmd_t *)dl->commands;
    for (size_t i = 0; i < count; i++) {
        switch (cmds[i].op) {
            case GL_DL_CMD_BEGIN:      glBegin(cmds[i].mode); break;
            case GL_DL_CMD_END:        glEnd(); break;
            case GL_DL_CMD_VERTEX4F:   glVertex4f(cmds[i].v[0], cmds[i].v[1], cmds[i].v[2], cmds[i].v[3]); break;
            case GL_DL_CMD_NORMAL3F:   glNormal3f(cmds[i].v[0], cmds[i].v[1], cmds[i].v[2]); break;
            case GL_DL_CMD_COLOR4F:    glColor4f(cmds[i].v[0], cmds[i].v[1], cmds[i].v[2], cmds[i].v[3]); break;
            case GL_DL_CMD_TEXCOORD4F: glTexCoord4f(cmds[i].v[0], cmds[i].v[1], cmds[i].v[2], cmds[i].v[3]); break;
            case GL_DL_CMD_SHADEMODEL: glShadeModel(cmds[i].mode); break;
            default: break;
        }
    }
    ctx->is_compiling_list = prev_comp;
}

void glCallLists(GLsizei n, GLenum type, const GLvoid *lists) {
    (void)type;
    if (n <= 0 || !lists) return;
    const GLuint *l = (const GLuint *)lists;
    for (GLsizei i = 0; i < n; i++) {
        glCallList(l[i]);
    }
}

void glDeleteLists(GLuint list, GLsizei range) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || list == 0) return;

    for (GLsizei i = 0; i < range; i++) {
        int idx = (int)(list + i) - 1;
        if (idx >= 0 && idx < ctx->list_count && ctx->display_lists[idx]) {
            ctx->display_lists[idx]->valid = false;
            if (ctx->display_lists[idx]->commands) {
                kfree(ctx->display_lists[idx]->commands);
                ctx->display_lists[idx]->commands = NULL;
            }
        }
    }
}

GLboolean glIsList(GLuint list) {
    GLContext *ctx = gl_get_current_context();
    if (!ctx || list == 0 || (int)list > ctx->list_count) return GL_FALSE;
    gl_display_list_t *dl = ctx->display_lists[list - 1];
    return (dl && dl->valid) ? GL_TRUE : GL_FALSE;
}

