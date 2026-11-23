#include <ipo/gfx.h>
#include <ipo/gfx3d.h>
#include <ipo/gpu_client.h>
#include <wm.h>
#include <syscall.h>
#include <system/timer.h>
#include <system/state.h>
#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 180
#define WIN_H 136

#define SC_ESC   0x01u
#define SC_Q     0x10u
#define SC_SPACE 0x39u
#define SC_F11   0x57u
#define SC_UP    0x48u
#define SC_DOWN  0x50u
#define SC_LEFT  0x4Bu
#define SC_RIGHT 0x4Du

static volatile bool app_running = true;
static volatile bool toggle_fs = false;
static volatile bool auto_spin = true;
static float rot_x = 18.0f;
static float rot_y = 26.0f;

static void build_studio_cube_texture_64f(ipo_gfx3d_texture_t *out, int sz) {
    out->width = sz;
    out->height = sz;
    out->texels = (ipo_rgba64f_t *)malloc((size_t)sz * (size_t)sz * sizeof(ipo_rgba64f_t));
    if (!out->texels) return;
    int half = sz / 2;
    for (int y = 0; y < sz; y++) {
        double v = ((double)y + 0.5) / (double)sz;
        double dv = fabs(v - 0.5);
        for (int x = 0; x < sz; x++) {
            double u = ((double)x + 0.5) / (double)sz;
            double du = fabs(u - 0.5);
            double dmax = (du > dv) ? du : dv;
            double fr, fg, fb;
            if (dmax > 0.405) {
                double t = (dmax - 0.405) / 0.095;
                double shade = 0.24 - 0.09 * t;
                fr = shade * 0.88; fg = shade * 0.94; fb = shade * 1.12;
            } else if (dmax > 0.386) {
                /* Precision brushed champagne-gold inlay hairline */
                fr = 1.02; fg = 0.80; fb = 0.34;
            } else {
                if (du < 0.010 || dv < 0.010) {
                    fr = 0.05; fg = 0.07; fb = 0.11;
                } else {
                    bool cyan_quad = ((x < half) ^ (y < half)) != 0;
                    double lx = fabs((double)(x % half) / (double)half - 0.5);
                    double ly = fabs((double)(y % half) / (double)half - 0.5);
                    double radial = 1.0 - 0.28 * (lx * lx + ly * ly);
                    if (cyan_quad) {
                        fr = 0.05 * radial; fg = 0.64 * radial; fb = 1.00 * radial;
                    } else {
                        fr = 0.98 * radial; fg = 0.40 * radial; fb = 0.06 * radial;
                    }
                }
            }
            out->texels[(size_t)y * (size_t)sz + (size_t)x] = (ipo_rgba64f_t){ fr, fg, fb, 1.0 };
        }
    }
}

static void build_obsidian_table_texture_64f(ipo_gfx3d_texture_t *out, int sz) {
    out->width = sz;
    out->height = sz;
    out->texels = (ipo_rgba64f_t *)malloc((size_t)sz * (size_t)sz * sizeof(ipo_rgba64f_t));
    if (!out->texels) return;
    int tile = sz / 4;
    for (int y = 0; y < sz; y++) {
        int ty = y / tile, ly = y % tile;
        double fy = (double)y / (double)sz;
        for (int x = 0; x < sz; x++) {
            int tx = x / tile, lx = x % tile;
            double fx = (double)x / (double)sz;
            bool odd = ((tx ^ ty) & 1) != 0;
            double vein = sin(fx * 18.8495 + fy * 12.5663 + 1.35 * sin(fy * 9.4247 - fx * 6.2831));
            vein = 0.5 + 0.5 * vein;
            vein = vein * vein;
            int ex = (lx < tile - 1 - lx) ? lx : (tile - 1 - lx);
            int ey = (ly < tile - 1 - ly) ? ly : (tile - 1 - ly);
            int edge = (ex < ey) ? ex : ey;
            double seam = (edge <= 1) ? 0.30 : ((edge == 2) ? 0.72 : 1.0);
            double r, g, b;
            if (!odd) {
                r = (0.16 + 0.07 * vein) * seam;
                g = (0.18 + 0.07 * vein) * seam;
                b = (0.23 + 0.09 * vein) * seam;
            } else {
                r = (0.065 + 0.045 * vein) * seam;
                g = (0.078 + 0.052 * vein) * seam;
                b = (0.108 + 0.068 * vein) * seam;
            }
            out->texels[(size_t)y * (size_t)sz + (size_t)x] = (ipo_rgba64f_t){ r, g, b, 1.0 };
        }
    }
}

static void on_window_event(wm_window_t *w, uint32_t event, uint32_t data) {
    (void)w;
    if (event == WM_EVENT_CLOSE) {
        app_running = false;
    } else if (event == WM_EVENT_KEY_DOWN) {
        uint8_t sc = (uint8_t)(data & 0xFFu);
        if (sc == SC_ESC || sc == SC_Q) {
            app_running = false;
        } else if (sc == SC_F11) {
            toggle_fs = true;
        } else if (sc == SC_SPACE) {
            auto_spin = !auto_spin;
        } else if (sc == SC_LEFT) {
            rot_y -= 7.0f; auto_spin = false;
        } else if (sc == SC_RIGHT) {
            rot_y += 7.0f; auto_spin = false;
        } else if (sc == SC_UP) {
            rot_x -= 7.0f; if (rot_x < -60.0f) rot_x = -60.0f; auto_spin = false;
        } else if (sc == SC_DOWN) {
            rot_x += 7.0f; if (rot_x > 60.0f) rot_x = 60.0f; auto_spin = false;
        }
    }
}

static void calc_render_size(wm_window_t *win, bool has_gpu, int *out_w, int *out_h) {
    ipo_gfx_caps_t c;
    memset(&c, 0, sizeof(c));
    if (ipo_gfx_get_caps(&c) == 0 && c.active_hw_bpp >= 15u) {
        if (win && win->fullscreen && c.active_width >= 320u && c.active_height >= 200u) {
            *out_w = (int)c.active_width;
            *out_h = (int)c.active_height;
        } else {
            int px = 0, py = 0, pw = 0, ph = 0;
            ipo_gfx_wm_window_phys_rect(win, &px, &py, &pw, &ph);
            *out_w = (pw > 32) ? pw : WIN_W;
            *out_h = (ph > 32) ? ph : WIN_H;
        }
        if (!has_gpu && *out_w > 600) {
            *out_h = (*out_h * 600) / (*out_w);
            *out_w = 600;
            if (*out_h < 180) *out_h = 180;
        }
        return;
    }
    *out_w = (win && win->w > 0) ? (int)win->w : WIN_W;
    *out_h = (win && win->h > 0) ? (int)win->h : WIN_H;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    uint32_t pid = (uint32_t)ipo_syscall(IPO_SYSCALL_GETPID, 0, NULL);
    if (pid == 0u) pid = 1u;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "3D GL Studio Cube";
    opt.x = (int16_t)(12 + (int)((pid * 28u) % 100u));
    opt.y = (int16_t)(14 + (int)((pid * 18u) % 45u));
    opt.w = WIN_W;
    opt.h = WIN_H;
    opt.decor_style = WM_DECOR_DEFAULT;

    (void)ipo_gfx_ensure_hd_mode(1280u, 720u);
    wm_window_t *win = wm_create_window(&opt);
    if (!win) return 1;

    ipo_gpu_info_t gi;
    bool has_gpu = (ipo_gpu_get_info(&gi) &&
                    gi.backend_mode == IPO_GPU_BACKEND_VIRGL_3D &&
                    !gi.forced_cpu_fallback);

    int ren_w = WIN_W, ren_h = WIN_H;
    calc_render_size(win, has_gpu, &ren_w, &ren_h);

    ipo_gfx3d_ctx_t *ctx = ipo_gfx3d_create(ren_w, ren_h);
    if (!ctx) {
        wm_destroy_window(win);
        return 1;
    }

    ipo_gfx3d_mesh_t m_cube, m_ring, m_floor;
    memset(&m_cube, 0, sizeof(m_cube));
    memset(&m_ring, 0, sizeof(m_ring));
    memset(&m_floor, 0, sizeof(m_floor));

    ipo_gfx3d_mesh_rounded_cube(&m_cube, 0.72, 0.16, has_gpu ? 20 : 12);
    ipo_gfx3d_mesh_torus(&m_ring, 1.32, 0.075, has_gpu ? 96 : 52, has_gpu ? 32 : 18);
    ipo_gfx3d_mesh_plane(&m_floor, 18.0, 18.0, has_gpu ? 18 : 8);
    for (uint32_t vi = 0u; vi < m_floor.vert_count; vi++) {
        m_floor.verts[vi].u *= 3.8;
        m_floor.verts[vi].v *= 3.8;
    }

    ipo_gfx3d_texture_t tex_cube, tex_floor;
    memset(&tex_cube, 0, sizeof(tex_cube));
    memset(&tex_floor, 0, sizeof(tex_floor));
    build_studio_cube_texture_64f(&tex_cube, 256);
    build_obsidian_table_texture_64f(&tex_floor, 256);

    ctx->ambient_sky = (ipo_rgba64f_t){ 0.24, 0.30, 0.44, 1.0 };
    ctx->ambient_ground = (ipo_rgba64f_t){ 0.06, 0.06, 0.09, 1.0 };
    ctx->enable_env_reflection = true;

    ipo_gfx3d_ensure_lights(ctx, 4u);
    ctx->lights[0].enabled = true;
    ctx->lights[0].is_point = false;
    ctx->lights[0].pos_or_dir = ipo_vec3(0.58, 0.80, 0.44);
    ctx->lights[0].color = (ipo_rgba64f_t){ 2.60, 2.40, 2.15, 1.0 };

    ctx->lights[1].enabled = true;
    ctx->lights[1].is_point = false;
    ctx->lights[1].pos_or_dir = ipo_vec3(-0.72, 0.45, -0.52);
    ctx->lights[1].color = (ipo_rgba64f_t){ 0.65, 0.95, 1.65, 1.0 };

    ctx->lights[2].enabled = true;
    ctx->lights[2].is_point = false;
    ctx->lights[2].pos_or_dir = ipo_vec3(0.68, 0.32, -0.66);
    ctx->lights[2].color = (ipo_rgba64f_t){ 0.72, 0.78, 0.94, 1.0 };

    ctx->lights[3].enabled = true;
    ctx->lights[3].is_point = true;
    ctx->lights[3].pos_or_dir = ipo_vec3(-1.8, 1.4, 2.0);
    ctx->lights[3].color = (ipo_rgba64f_t){ 1.15, 0.78, 0.48, 1.0 };

    const double floor_y = -1.25;
    uint16_t last_ww = win->w, last_wh = win->h;
    bool last_fs = win->fullscreen;
    bool mouse_dragging = false;
    int last_mx = 0, last_my = 0;

    uint32_t prev_tick = timer_millis();
    uint32_t fps_t0 = prev_tick, fps_n = 0u, yield_t0 = prev_tick;
    int cur_fps = 0;

    while (app_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        wm_event_t ev;
        while (wm_poll_event(win, &ev)) {
            on_window_event(win, ev.type, ev.data);
        }
        if (toggle_fs) {
            toggle_fs = false;
            wm_set_fullscreen(win, !win->fullscreen);
        }
        if (!app_running || !wm_is_window_valid(win)) break;

        if (win->w != last_ww || win->h != last_wh || win->fullscreen != last_fs) {
            last_ww = win->w; last_wh = win->h; last_fs = win->fullscreen;
            int nw = ren_w, nh = ren_h;
            calc_render_size(win, has_gpu, &nw, &nh);
            if ((nw != ctx->width || nh != ctx->height) && ipo_gfx3d_resize(ctx, nw, nh)) {
                ren_w = nw; ren_h = nh;
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
                        last_mx = ms.x; last_my = ms.y;
                    }
                } else {
                    int dx = ms.x - last_mx;
                    int dy = ms.y - last_my;
                    if (dx != 0 || dy != 0) {
                        rot_y += (float)dx * 1.3f;
                        rot_x += (float)dy * 1.3f;
                        if (rot_x < -60.0f) rot_x = -60.0f;
                        if (rot_x > 60.0f)  rot_x = 60.0f;
                        auto_spin = false;
                        last_mx = ms.x; last_my = ms.y;
                    }
                }
            } else {
                mouse_dragging = false;
            }
        } else {
            mouse_dragging = false;
        }

        uint32_t now = timer_millis();
        uint32_t dt_ms = now - prev_tick;
        prev_tick = now;
        if (dt_ms > 100u) dt_ms = 100u;
        if (auto_spin) {
            double dt_s = (double)dt_ms * 0.001;
            rot_y += (float)(42.0 * dt_s);
            rot_x = 18.0f + 12.0f * sinf(rot_y * 0.01745329f * 0.55f);
            if (rot_y >= 36000.0f) rot_y -= 36000.0f;
        }

        ipo_gfx3d_clear(ctx,
                        (ipo_rgba64f_t){ 0.025, 0.036, 0.072, 1.0 },
                        (ipo_rgba64f_t){ 0.095, 0.125, 0.200, 1.0 },
                        1.0);

        ipo_vec3_t eye = ipo_vec3(0.0, 1.42, 4.45);
        ipo_vec3_t tgt = ipo_vec3(0.0, -0.08, 0.0);
        ctx->cam_pos = eye;
        ctx->view = ipo_mat4_look_at(eye, tgt, ipo_vec3(0.0, 1.0, 0.0));
        ctx->proj = ipo_mat4_perspective(42.0 * (3.1415926535 / 180.0),
                                         (double)ctx->width / (double)ctx->height, 0.2, 45.0);

        double rx_r = (double)rot_x * (3.1415926535 / 180.0);
        double ry_r = (double)rot_y * (3.1415926535 / 180.0);
        ipo_mat4_t r_cube_y = ipo_mat4_rotate_y(ry_r);
        ipo_mat4_t r_cube_x = ipo_mat4_rotate_x(rx_r);
        ipo_mat4_t r_cube = ipo_mat4_mul(&r_cube_y, &r_cube_x);
        ipo_mat4_t t_center = ipo_mat4_translate(0.0, 0.18 + 0.05 * sin(ry_r * 1.2), 0.0);
        ipo_mat4_t mod_cube = ipo_mat4_mul(&t_center, &r_cube);

        ipo_mat4_t r_ring_z = ipo_mat4_rotate_z(0.55 * sin(ry_r * 0.65));
        ipo_mat4_t r_ring_y = ipo_mat4_rotate_y(-ry_r * 0.85);
        ipo_mat4_t r_ring_tilt = ipo_mat4_rotate_x(1.18);
        ipo_mat4_t r_ring = ipo_mat4_mul(&r_ring_z, &r_ring_y);
        r_ring = ipo_mat4_mul(&r_ring, &r_ring_tilt);
        ipo_mat4_t mod_ring = ipo_mat4_mul(&t_center, &r_ring);

        ipo_mat4_t mod_floor = ipo_mat4_translate(0.0, floor_y, 0.0);

        ipo_gfx3d_material_t mat_cube = {
            .albedo = { 1.0, 1.0, 1.0, 1.0 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.34, .roughness = 0.14, .reflectivity = 0.88,
            .clearcoat = 0.92, .ao = 1.0, .albedo_tex = &tex_cube
        };
        ipo_gfx3d_material_t mat_ring = {
            .albedo = { 0.94, 0.76, 0.42, 1.0 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.98, .roughness = 0.11, .reflectivity = 1.25,
            .clearcoat = 0.55, .ao = 1.0, .albedo_tex = NULL
        };
        ipo_gfx3d_material_t mat_floor = {
            .albedo = { 1.0, 1.0, 1.0, 0.62 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.14, .roughness = 0.08, .reflectivity = 0.78,
            .clearcoat = 0.92, .ao = 1.0, .albedo_tex = &tex_floor
        };

        /* 1. Planar mirror reflection pass below floor_y */
        ipo_gfx3d_draw_mirrored_mesh(ctx, &m_cube, &mod_cube, &mat_cube, floor_y, 0.44);
        ipo_gfx3d_draw_mirrored_mesh(ctx, &m_ring, &mod_ring, &mat_ring, floor_y, 0.44);
        ipo_gfx3d_clear_depth(ctx, 1.0);

        /* 2. Translucent glossy obsidian marble studio floor */
        ipo_gfx3d_draw_mesh(ctx, &m_floor, &mod_floor, &mat_floor);

        /* 3. Soft 5-tap Poisson penumbra shadows */
        ipo_gfx3d_draw_shadow_projection(ctx, &m_cube, &mod_cube, ctx->lights[0].pos_or_dir, floor_y, 0.46);
        ipo_gfx3d_draw_shadow_projection(ctx, &m_ring, &mod_ring, ctx->lights[0].pos_or_dir, floor_y, 0.36);

        /* 4. Foreground sculpture */
        ipo_gfx3d_draw_mesh(ctx, &m_cube, &mod_cube, &mat_cube);
        ipo_gfx3d_draw_mesh(ctx, &m_ring, &mod_ring, &mat_ring);

        /* 5. CPU Bloom + Perceptual FXAA before crisp studio HUD strip */
        ipo_gfx3d_apply_postfx(ctx, 0.16, true);

        if (ctx->width >= 200 && ctx->height >= 120) {
            ipo_gfx3d_draw_rect2d(ctx, 0, ctx->height - 14, ctx->width, 14, (ipo_rgba64f_t){ 0.015, 0.02, 0.04, 0.80 });
            const char *mname = has_gpu ? "VirGL MSAA" : "RGBA64F";
            char hud[96];
            snprintf(hud, sizeof(hud), "%s | %d FPS | %dx%d | F11:FS", mname, cur_fps, ctx->width, ctx->height);
            ipo_gfx3d_draw_text2d(ctx, 6, ctx->height - 10, hud, (ipo_rgba64f_t){ 0.90, 0.94, 1.0, 1.0 }, 1);
        }

        ipo_gfx3d_present(ctx, win, IPO_TONEMAP_ACES, 1.06);
        wm_compositor_tick();

        fps_n++;
        uint32_t dt = now - fps_t0;
        if (dt >= 1000u) {
            cur_fps = (int)((fps_n * 1000u + dt / 2u) / dt);
            extern int serial_printf(const char *fmt, ...);
            serial_printf("[gl_cube] %u FPS (%dx%d)\n", (unsigned)cur_fps, ren_w, ren_h);
            fps_n = 0u;
            fps_t0 = now;
        }
        if (now - yield_t0 >= 10u) {
            yield_t0 = now;
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        }
    }

    ipo_gfx3d_tex_free(&tex_cube);
    ipo_gfx3d_tex_free(&tex_floor);
    ipo_gfx3d_mesh_free(&m_cube);
    ipo_gfx3d_mesh_free(&m_ring);
    ipo_gfx3d_mesh_free(&m_floor);
    ipo_gfx3d_destroy(ctx);
    if (wm_is_window_valid(win)) wm_destroy_window(win);
    return 0;
}
