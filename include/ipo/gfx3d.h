#ifndef IPO_PUBLIC_GFX3D_H
#define IPO_PUBLIC_GFX3D_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <ipo/gfx.h>
#include <driver/color.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPO_GFX3D_API_VERSION 0x00010000u

typedef struct {
    double x, y, z;
} ipo_vec3_t;

typedef struct {
    double x, y, z, w;
} ipo_vec4_t;

/* Column-major 4x4 matrix */
typedef struct {
    double m[16];
} ipo_mat4_t;

typedef struct {
    ipo_vec3_t pos;
    ipo_vec3_t normal;
    double     u, v;
    ipo_rgba64f_t color;
} ipo_gfx3d_vertex_t;

typedef struct {
    int                   width;
    int                   height;
    ipo_rgba64f_t        *texels; /* width * height RGBA64F linear */
} ipo_gfx3d_texture_t;

typedef struct {
    ipo_rgba64f_t               albedo;
    ipo_rgba64f_t               emissive;
    double                      metallic;      /* 0..1 */
    double                      roughness;     /* 0.02..1 */
    double                      reflectivity;  /* 0..1 env reflection multiplier */
    double                      clearcoat;     /* 0..1 */
    double                      ao;            /* 0..1 */
    const ipo_gfx3d_texture_t  *albedo_tex;
} ipo_gfx3d_material_t;

typedef struct {
    bool          enabled;
    bool          is_point;      /* false = directional (dir = normalize(pos)) */
    ipo_vec3_t    pos_or_dir;    /* world position or vector toward light */
    ipo_rgba64f_t color;         /* HDR linear radiance */
} ipo_gfx3d_light_t;

typedef enum {
    IPO_GFX3D_CULL_NONE = 0,
    IPO_GFX3D_CULL_BACK = 1,
    IPO_GFX3D_CULL_FRONT = 2
} ipo_gfx3d_cull_t;

typedef struct {
    uint32_t             struct_size;
    int                  width;
    int                  height;
    ipo_rgba64f_t       *color_fb;     /* Native 256-bpp RGBA64F framebuffer */
    float               *z_buffer;     /* Per-pixel 32-bit float depth [0..1] */
    ipo_mat4_t           view;
    ipo_mat4_t           proj;
    ipo_vec3_t           cam_pos;
    ipo_gfx3d_cull_t     cull_mode;
    bool                 depth_test;
    bool                 depth_write;
    bool                 enable_env_reflection;
    bool                 enable_floor_shadow;
    double               floor_y;
    ipo_gfx3d_light_t   *lights;
    uint32_t             lights_cap;
    ipo_rgba64f_t        ambient_sky;
    ipo_rgba64f_t        ambient_ground;
    uint64_t             tris_submitted;
    uint64_t             tris_drawn;
} ipo_gfx3d_ctx_t;

typedef struct {
    ipo_gfx3d_vertex_t *verts;
    uint32_t            vert_count;
    uint32_t           *indices;
    uint32_t            index_count;
} ipo_gfx3d_mesh_t;

/* Matrix & Vector API */
ipo_vec3_t ipo_vec3(double x, double y, double z);
ipo_vec3_t ipo_vec3_add(ipo_vec3_t a, ipo_vec3_t b);
ipo_vec3_t ipo_vec3_sub(ipo_vec3_t a, ipo_vec3_t b);
ipo_vec3_t ipo_vec3_scale(ipo_vec3_t v, double s);
double     ipo_vec3_dot(ipo_vec3_t a, ipo_vec3_t b);
ipo_vec3_t ipo_vec3_cross(ipo_vec3_t a, ipo_vec3_t b);
ipo_vec3_t ipo_vec3_norm(ipo_vec3_t v);

ipo_mat4_t ipo_mat4_identity(void);
ipo_mat4_t ipo_mat4_mul(const ipo_mat4_t *a, const ipo_mat4_t *b);
ipo_mat4_t ipo_mat4_translate(double x, double y, double z);
ipo_mat4_t ipo_mat4_scale(double sx, double sy, double sz);
ipo_mat4_t ipo_mat4_rotate_x(double rad);
ipo_mat4_t ipo_mat4_rotate_y(double rad);
ipo_mat4_t ipo_mat4_rotate_z(double rad);
ipo_mat4_t ipo_mat4_perspective(double fovy_rad, double aspect, double znear, double zfar);
ipo_mat4_t ipo_mat4_ortho(double left, double right, double bottom, double top, double znear, double zfar);
ipo_mat4_t ipo_mat4_look_at(ipo_vec3_t eye, ipo_vec3_t target, ipo_vec3_t up);

/* 3D Context & Rendering API */
ipo_gfx3d_ctx_t *ipo_gfx3d_create(int width, int height);
void             ipo_gfx3d_destroy(ipo_gfx3d_ctx_t *ctx);
bool             ipo_gfx3d_resize(ipo_gfx3d_ctx_t *ctx, int width, int height);
bool             ipo_gfx3d_ensure_lights(ipo_gfx3d_ctx_t *ctx, uint32_t min_lights);
bool             ipo_gfx3d_set_light(ipo_gfx3d_ctx_t *ctx, uint32_t index, const ipo_gfx3d_light_t *light);
void             ipo_gfx3d_clear(ipo_gfx3d_ctx_t *ctx, ipo_rgba64f_t sky_top, ipo_rgba64f_t sky_horizon, double depth);
void             ipo_gfx3d_clear_depth(ipo_gfx3d_ctx_t *ctx, double depth);
void             ipo_gfx3d_draw_mesh(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                                     const ipo_mat4_t *model, const ipo_gfx3d_material_t *mat);
void             ipo_gfx3d_draw_mirrored_mesh(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                                              const ipo_mat4_t *model, const ipo_gfx3d_material_t *mat,
                                              double plane_y, double reflectivity_tint);
void             ipo_gfx3d_draw_shadow_projection(ipo_gfx3d_ctx_t *ctx, const ipo_gfx3d_mesh_t *mesh,
                                                  const ipo_mat4_t *model, ipo_vec3_t light_dir,
                                                  double plane_y, double opacity);
void             ipo_gfx3d_apply_postfx(ipo_gfx3d_ctx_t *ctx, double bloom_strength, bool enable_aa);

/* Procedural 3D Meshes & Textures */
bool ipo_gfx3d_mesh_cube(ipo_gfx3d_mesh_t *out_mesh, double half_extent);
bool ipo_gfx3d_mesh_rounded_cube(ipo_gfx3d_mesh_t *out_mesh, double half_extent, double corner_radius, int subdiv);
bool ipo_gfx3d_mesh_gear(ipo_gfx3d_mesh_t *out_mesh, double inner_radius, double outer_radius,
                         double width, int teeth, double tooth_depth, double chamfer);
bool ipo_gfx3d_mesh_sphere(ipo_gfx3d_mesh_t *out_mesh, double radius, int stacks, int slices);
bool ipo_gfx3d_mesh_torus(ipo_gfx3d_mesh_t *out_mesh, double major_r, double minor_r, int segs, int sides);
bool ipo_gfx3d_mesh_plane(ipo_gfx3d_mesh_t *out_mesh, double half_w, double half_d, int subdiv);
void ipo_gfx3d_mesh_free(ipo_gfx3d_mesh_t *mesh);

bool ipo_gfx3d_tex_checker(ipo_gfx3d_texture_t *out_tex, int w, int h, int check_px,
                           ipo_rgba64f_t c0, ipo_rgba64f_t c1);
void ipo_gfx3d_tex_free(ipo_gfx3d_texture_t *tex);

/* 2D Composited Overlays in Linear RGBA64F onto the Same 3D Scene */
void ipo_gfx3d_draw_rect2d(ipo_gfx3d_ctx_t *ctx, int x, int y, int w, int h, ipo_rgba64f_t premul_col);
void ipo_gfx3d_draw_line2d(ipo_gfx3d_ctx_t *ctx, int x0, int y0, int x1, int y1, ipo_rgba64f_t premul_col);
void ipo_gfx3d_draw_text2d(ipo_gfx3d_ctx_t *ctx, int x, int y, const char *text,
                           ipo_rgba64f_t color, int scale);

/* Present RGBA64F 3D+2D Scene via Public <ipo/gfx.h> Syscalls */
int ipo_gfx3d_present(ipo_gfx3d_ctx_t *ctx, wm_window_t *win,
                      ipo_tonemap_op_t tonemap, double exposure);

#ifdef __cplusplus
}
#endif

#endif /* IPO_PUBLIC_GFX3D_H */
