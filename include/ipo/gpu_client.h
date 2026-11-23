#ifndef IPO_PUBLIC_GPU_CLIENT_H
#define IPO_PUBLIC_GPU_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <ipo/gpu.h>
#include <wm.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ipo_gpu_cmd_t *cmds;
    uint32_t       cmd_count;
    uint32_t       cmd_cap;
    uint8_t       *const_blob;
    uint32_t       const_used;
    uint32_t       const_cap;
} ipo_gpu_batch_t;

typedef struct {
    uint32_t res_id;
    uint32_t byte_cap;
} ipo_gpu_dynbuf_t;

bool     ipo_gpu_get_info(ipo_gpu_info_t *out_info);
int      ipo_gpu_set_mode(uint32_t force_cpu);
bool     ipo_gpu_3d_available(void);
bool     ipo_gpu_2d_available(void);
uint32_t ipo_gpu_max_samples(void);
bool     ipo_gpu_create_studio_env(uint32_t ctx_id, uint32_t *out_sharp_res, uint32_t *out_soft_res);

uint32_t ipo_gpu_ctx_create(const char *debug_name);
int      ipo_gpu_ctx_destroy(uint32_t ctx_id);

uint32_t ipo_gpu_res_create(uint32_t ctx_id, const ipo_gpu_res_desc_t *desc);
uint32_t ipo_gpu_res_create_buf(uint32_t ctx_id, uint32_t kind, const void *init_data, uint32_t byte_size);
uint32_t ipo_gpu_res_create_tex2d(uint32_t ctx_id, uint32_t format, uint32_t w, uint32_t h,
                                  const void *pixels, uint32_t stride_bytes);
uint32_t ipo_gpu_res_create_rt(uint32_t ctx_id, uint32_t format, uint32_t w, uint32_t h, uint32_t extra_flags);
int      ipo_gpu_res_upload_buf(uint32_t ctx_id, uint32_t res_id, uint32_t offset_bytes,
                                const void *data, uint32_t byte_size);
int      ipo_gpu_res_upload_tex2d(uint32_t ctx_id, uint32_t res_id,
                                  uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  uint32_t stride_bytes, const void *pixels);
int      ipo_gpu_res_destroy(uint32_t res_id);

/* Growable GPU dynamic vertex/index buffer helper */
bool     ipo_gpu_dynbuf_upload(uint32_t ctx_id, ipo_gpu_dynbuf_t *db, uint32_t kind,
                               const void *data, uint32_t byte_size);
void     ipo_gpu_dynbuf_destroy(ipo_gpu_dynbuf_t *db);

/* Dynamic GPU command batch builder */
void     ipo_gpu_batch_init(ipo_gpu_batch_t *b);
void     ipo_gpu_batch_reset(ipo_gpu_batch_t *b);
void     ipo_gpu_batch_free(ipo_gpu_batch_t *b);
bool     ipo_gpu_batch_push(ipo_gpu_batch_t *b, const ipo_gpu_cmd_t *cmd);
bool     ipo_gpu_cmd_set_target(ipo_gpu_batch_t *b, uint32_t color_res, uint32_t depth_res, uint32_t w, uint32_t h);
bool     ipo_gpu_cmd_clear(ipo_gpu_batch_t *b, uint32_t mask, const float rgba[4], float depth);
bool     ipo_gpu_cmd_set_viewport(ipo_gpu_batch_t *b, float x, float y, float w, float h, float znear, float zfar);
bool     ipo_gpu_cmd_set_scissor(ipo_gpu_batch_t *b, bool enable, int32_t x, int32_t y, int32_t w, int32_t h);
bool     ipo_gpu_cmd_set_pipeline(ipo_gpu_batch_t *b, uint32_t program, uint8_t cull_mode, bool front_ccw,
                                  bool depth_test, bool depth_write, uint8_t depth_func,
                                  uint8_t blend_mode, bool wireframe);
bool     ipo_gpu_cmd_set_pipeline_ex(ipo_gpu_batch_t *b, uint32_t program, uint8_t cull_mode, bool front_ccw,
                                     bool depth_test, bool depth_write, uint8_t depth_func,
                                     uint8_t blend_mode, uint8_t blend_eq,
                                     uint8_t blend_src, uint8_t blend_dst,
                                     bool wireframe, float point_size, float line_width);
bool     ipo_gpu_cmd_set_constants(ipo_gpu_batch_t *b, uint32_t shader_stage, const float *floats, uint32_t float_count);
bool     ipo_gpu_cmd_bind_tex(ipo_gpu_batch_t *b, uint32_t slot, uint32_t tex_res, bool linear, bool wrap_repeat);
bool     ipo_gpu_cmd_draw(ipo_gpu_batch_t *b, uint32_t vbo_res, uint32_t ibo_res, uint32_t index_size,
                          uint32_t prim_mode, uint32_t start, uint32_t count,
                          uint32_t vbo_offset_bytes, uint32_t ibo_offset_bytes);
bool     ipo_gpu_cmd_blit(ipo_gpu_batch_t *b, uint32_t dst_res, uint32_t src_res,
                          int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                          int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                          bool linear, bool alpha_blend);
bool     ipo_gpu_cmd_readback(ipo_gpu_batch_t *b, uint32_t res_id, uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h, void *dst_ptr, uint32_t dst_stride_bytes);
bool     ipo_gpu_cmd_present_window(ipo_gpu_batch_t *b, uint32_t res_id, wm_window_t *win,
                                    uint32_t src_w, uint32_t src_h);
bool     ipo_gpu_cmd_present_screen(ipo_gpu_batch_t *b, uint32_t res_id, uint32_t src_w, uint32_t src_h);
int      ipo_gpu_batch_flush(ipo_gpu_batch_t *b, uint32_t ctx_id);

#ifdef __cplusplus
}
#endif

#endif /* IPO_PUBLIC_GPU_CLIENT_H */
