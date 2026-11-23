#ifndef IPO_PUBLIC_GPU_H
#define IPO_PUBLIC_GPU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <ipo/gfx.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPO_GPU_API_VERSION 0x00010000u

typedef enum {
    IPO_GPU_BACKEND_CPU_FALLBACK = 0,
    IPO_GPU_BACKEND_VIRTIO_2D    = 1,
    IPO_GPU_BACKEND_VIRGL_3D     = 2
} ipo_gpu_backend_mode_t;

typedef enum {
    IPO_GPU_RES_VERTEX_BUF   = 0,
    IPO_GPU_RES_INDEX_BUF    = 1,
    IPO_GPU_RES_TEX_2D       = 2,
    IPO_GPU_RES_COLOR_TARGET = 3,
    IPO_GPU_RES_DEPTH_TARGET = 4
} ipo_gpu_res_kind_t;

typedef enum {
    IPO_GPU_FMT_NONE    = 0,
    IPO_GPU_FMT_BGRA8   = 1,
    IPO_GPU_FMT_BGRX8   = 2,
    IPO_GPU_FMT_RGBA8   = 3,
    IPO_GPU_FMT_RGBA16F = 4,
    IPO_GPU_FMT_RGBA32F = 5,
    IPO_GPU_FMT_Z32F    = 6,
    IPO_GPU_FMT_R32F    = 7,
    IPO_GPU_FMT_RAW     = 8
} ipo_gpu_format_t;

#define IPO_GPU_RES_FLAG_RENDER_TARGET (1u << 0)
#define IPO_GPU_RES_FLAG_SAMPLER       (1u << 1)
#define IPO_GPU_RES_FLAG_DEPTH_STENCIL (1u << 2)
#define IPO_GPU_RES_FLAG_SCANOUT       (1u << 3)
#define IPO_GPU_RES_FLAG_MSAA_SHIFT    4u
#define IPO_GPU_RES_FLAG_MSAA_MASK     (0xFu << IPO_GPU_RES_FLAG_MSAA_SHIFT)
#define IPO_GPU_RES_FLAG_MSAA(n)       (((uint32_t)(n) & 0xFu) << IPO_GPU_RES_FLAG_MSAA_SHIFT)

typedef enum {
    IPO_GPU_PROG_SOLID2D       = 0,
    IPO_GPU_PROG_QUAD2D        = 1,
    IPO_GPU_PROG_TONEMAP       = 2,
    IPO_GPU_PROG_STD3D         = 3,
    IPO_GPU_PROG_PBR3D         = 4,
    IPO_GPU_PROG_SHADOW        = 5,
    IPO_GPU_PROG_BLOOM_EXTRACT = 6,
    IPO_GPU_PROG_BLOOM_BLUR    = 7,
    IPO_GPU_PROG_COUNT
} ipo_gpu_program_t;

typedef enum {
    IPO_GPU_PRIM_POINTS    = 0,
    IPO_GPU_PRIM_LINES     = 1,
    IPO_GPU_PRIM_TRIANGLES = 4
} ipo_gpu_prim_t;

typedef enum {
    IPO_GPU_CULL_NONE  = 0,
    IPO_GPU_CULL_FRONT = 1,
    IPO_GPU_CULL_BACK  = 2
} ipo_gpu_cull_t;

typedef enum {
    IPO_GPU_CMP_NEVER   = 0,
    IPO_GPU_CMP_LESS    = 1,
    IPO_GPU_CMP_EQUAL   = 2,
    IPO_GPU_CMP_LEQUAL  = 3,
    IPO_GPU_CMP_GREATER = 4,
    IPO_GPU_CMP_NOTEQ   = 5,
    IPO_GPU_CMP_GEQUAL  = 6,
    IPO_GPU_CMP_ALWAYS  = 7
} ipo_gpu_cmp_t;

typedef enum {
    IPO_GPU_BLEND_NONE       = 0,
    IPO_GPU_BLEND_ALPHA      = 1, /* src_a, 1 - src_a */
    IPO_GPU_BLEND_ADDITIVE   = 2, /* src_a, 1 */
    IPO_GPU_BLEND_PREMUL     = 3, /* 1, 1 - src_a */
    IPO_GPU_BLEND_MULTIPLY   = 4, /* dst_col, 0 */
    IPO_GPU_BLEND_CUSTOM     = 5  /* uses blend_eq, blend_src, blend_dst */
} ipo_gpu_blend_t;

typedef enum {
    IPO_GPU_BEQ_ADD          = 0,
    IPO_GPU_BEQ_SUB          = 1,
    IPO_GPU_BEQ_REV_SUB      = 2,
    IPO_GPU_BEQ_MIN          = 3,
    IPO_GPU_BEQ_MAX          = 4
} ipo_gpu_blend_eq_t;

/* Values match Gallium PIPE_BLENDFACTOR_* directly */
typedef enum {
    IPO_GPU_BF_DEFAULT            = 0,
    IPO_GPU_BF_ONE                = 0x01,
    IPO_GPU_BF_SRC_COLOR          = 0x02,
    IPO_GPU_BF_SRC_ALPHA          = 0x03,
    IPO_GPU_BF_DST_ALPHA          = 0x04,
    IPO_GPU_BF_DST_COLOR          = 0x05,
    IPO_GPU_BF_SRC_ALPHA_SATURATE = 0x06,
    IPO_GPU_BF_ZERO               = 0x11,
    IPO_GPU_BF_INV_SRC_COLOR      = 0x12,
    IPO_GPU_BF_INV_SRC_ALPHA      = 0x13,
    IPO_GPU_BF_INV_DST_ALPHA      = 0x14,
    IPO_GPU_BF_INV_DST_COLOR      = 0x15
} ipo_gpu_blend_factor_t;

#define IPO_GPU_CLEAR_COLOR (1u << 0)
#define IPO_GPU_CLEAR_DEPTH (1u << 1)

/* Unified 48-byte vertex shared across 2D quads, GL 3D, and PBR 3D */
typedef struct {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float r, g, b, a;
} ipo_gpu_vertex_t;

typedef struct {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t backend_mode;        /* ipo_gpu_backend_mode_t */
    uint8_t  hw_detected;
    uint8_t  virgl_3d_supported;
    uint8_t  primary_scanout;
    uint8_t  forced_cpu_fallback;
    uint8_t  pci_bus;
    uint8_t  pci_slot;
    uint8_t  pci_func;
    uint8_t  reserved0;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t num_scanouts;
    uint32_t capset_id;
    uint32_t capset_version;
    uint32_t capset_max_size;
    uint64_t vram_bytes;
    uint64_t gpu_draw_calls;
    uint64_t gpu_blits;
    uint64_t gpu_cmds_submitted;
    uint64_t cpu_fallbacks;
    char    *name;
    uint32_t name_cap;
    uint32_t name_len;
} ipo_gpu_info_t;

typedef struct {
    uint32_t struct_size;
    uint32_t kind;         /* ipo_gpu_res_kind_t */
    uint32_t format;       /* ipo_gpu_format_t */
    uint32_t flags;        /* IPO_GPU_RES_FLAG_* */
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes; /* 0 = tight row stride */
    uint32_t byte_size;    /* required for VERTEX_BUF / INDEX_BUF */
} ipo_gpu_res_desc_t;

typedef enum {
    IPO_GPU_CMD_NOP = 0,
    IPO_GPU_CMD_SET_TARGET,
    IPO_GPU_CMD_CLEAR,
    IPO_GPU_CMD_SET_VIEWPORT,
    IPO_GPU_CMD_SET_SCISSOR,
    IPO_GPU_CMD_SET_PIPELINE,
    IPO_GPU_CMD_SET_CONSTANTS,
    IPO_GPU_CMD_BIND_TEXTURE,
    IPO_GPU_CMD_DRAW,
    IPO_GPU_CMD_BLIT,
    IPO_GPU_CMD_READBACK,
    IPO_GPU_CMD_PRESENT_WINDOW,
    IPO_GPU_CMD_PRESENT_SCREEN
} ipo_gpu_cmd_op_t;

typedef struct {
    uint32_t op;
    uint32_t flags;
    union {
        struct {
            uint32_t color_res;
            uint32_t depth_res;
            uint32_t width;
            uint32_t height;
        } target;
        struct {
            uint32_t mask;
            float    r, g, b, a;
            float    depth;
        } clear;
        struct {
            float    x, y, w, h;
            float    znear, zfar;
        } viewport;
        struct {
            uint32_t enable;
            int32_t  x, y, w, h;
        } scissor;
        struct {
            uint32_t program;    /* ipo_gpu_program_t */
            uint8_t  cull_mode;  /* ipo_gpu_cull_t */
            uint8_t  front_ccw;
            uint8_t  depth_test;
            uint8_t  depth_write;
            uint8_t  depth_func; /* ipo_gpu_cmp_t */
            uint8_t  blend_mode; /* ipo_gpu_blend_t */
            uint8_t  wireframe;
            uint8_t  blend_eq;   /* ipo_gpu_blend_eq_t */
            uint8_t  blend_src;  /* ipo_gpu_blend_factor_t */
            uint8_t  blend_dst;  /* ipo_gpu_blend_factor_t */
            uint16_t reserved1;
            float    point_size; /* <= 0.0f defaults to 1.0f */
            float    line_width; /* <= 0.0f defaults to 1.0f */
        } pipeline;
        struct {
            uint32_t shader_stage; /* 0 = VS, 1 = FS */
            uint32_t blob_offset_bytes;
            uint32_t float_count;
            uint32_t reserved;
        } constants;
        struct {
            uint32_t slot;
            uint32_t tex_res;
            uint8_t  filter_linear;
            uint8_t  wrap_repeat;
            uint16_t reserved;
        } bind_tex;
        struct {
            uint32_t vbo_res;
            uint32_t ibo_res;
            uint32_t index_size;   /* 0 = non-indexed, 2 = u16, 4 = u32 */
            uint32_t prim_mode;    /* ipo_gpu_prim_t */
            uint32_t start;
            uint32_t count;
            uint32_t vbo_offset_bytes;
            uint32_t ibo_offset_bytes;
        } draw;
        struct {
            uint32_t dst_res;
            uint32_t src_res;
            int32_t  dst_x, dst_y, dst_w, dst_h;
            int32_t  src_x, src_y, src_w, src_h;
            uint8_t  filter_linear;
            uint8_t  alpha_blend;
            uint16_t reserved;
        } blit;
        struct {
            uint32_t res_id;
            uint32_t x, y, w, h;
            uint64_t dst_user_ptr;
            uint32_t dst_stride_bytes;
            uint32_t reserved;
        } readback;
        struct {
            uint32_t res_id;
            uint64_t wm_window_ptr;
            uint32_t src_w, src_h;
            uint32_t dst_x, dst_y, dst_w, dst_h;
        } present;
    } u;
} ipo_gpu_cmd_t;

typedef struct {
    uint32_t             struct_size;
    uint32_t             ctx_id;
    uint32_t             res_id;
    uint32_t             offset_bytes;
    uint32_t             x, y, w, h;
    uint32_t             stride_bytes;
    uint32_t             byte_size;
    const void          *data;
} ipo_gpu_upload_desc_t;

typedef struct {
    uint32_t             struct_size;
    uint32_t             ctx_id;
    const ipo_gpu_cmd_t *cmds;
    uint32_t             cmd_count;
    const void          *const_blob;
    uint32_t             const_blob_bytes;
} ipo_gpu_batch_desc_t;

#ifdef __cplusplus
}
#endif

#endif /* IPO_PUBLIC_GPU_H */
