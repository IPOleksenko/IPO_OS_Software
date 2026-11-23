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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WIN_W 188
#define WIN_H 140

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
static float cam_yaw = 18.0f;
static float cam_pitch = 14.0f;

static void build_bench_texture_64f(ipo_gfx3d_texture_t *out, int sz) {
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
            bool alt = ((tx ^ ty) & 1) != 0;
            double grain = 0.5 + 0.5 * sin(fx * 25.1327 + fy * 12.5663 + 1.1 * cos(fy * 18.8495 - fx * 6.2831));
            int ex = (lx < tile - 1 - lx) ? lx : (tile - 1 - lx);
            int ey = (ly < tile - 1 - ly) ? ly : (tile - 1 - ly);
            int edge = (ex < ey) ? ex : ey;
            double groove = (edge <= 1) ? 0.34 : ((edge == 2) ? 0.74 : 1.0);
            double r, g, b;
            if (!alt) {
                r = (0.12 + 0.05 * grain) * groove;
                g = (0.14 + 0.06 * grain) * groove;
                b = (0.18 + 0.08 * grain) * groove;
            } else {
                r = (0.055 + 0.035 * grain) * groove;
                g = (0.068 + 0.042 * grain) * groove;
                b = (0.095 + 0.055 * grain) * groove;
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
            cam_yaw -= 6.0f;
        } else if (sc == SC_RIGHT) {
            cam_yaw += 6.0f;
        } else if (sc == SC_UP) {
            cam_pitch -= 5.0f; if (cam_pitch < 2.0f) cam_pitch = 2.0f;
        } else if (sc == SC_DOWN) {
            cam_pitch += 5.0f; if (cam_pitch > 48.0f) cam_pitch = 48.0f;
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
    opt.title = "3D GL Studio Gears";
    opt.x = (int16_t)(20 + (int)((pid * 24u) % 120u));
    opt.y = (int16_t)(15 + (int)((pid * 18u) % 65u));
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

    ipo_gfx3d_mesh_t g_ruby, g_emerald, g_gold, m_collar, m_floor;
    memset(&g_ruby, 0, sizeof(g_ruby));
    memset(&g_emerald, 0, sizeof(g_emerald));
    memset(&g_gold, 0, sizeof(g_gold));
    memset(&m_collar, 0, sizeof(m_collar));
    memset(&m_floor, 0, sizeof(m_floor));

    ipo_gfx3d_mesh_gear(&g_ruby,    0.62, 2.05, 0.58, 24, 0.34, 0.055);
    ipo_gfx3d_mesh_gear(&g_emerald, 0.34, 1.025, 0.96, 12, 0.33, 0.048);
    ipo_gfx3d_mesh_gear(&g_gold,    0.36, 1.025, 0.68, 12, 0.33, 0.048);
    ipo_gfx3d_mesh_torus(&m_collar, 0.74, 0.085, has_gpu ? 64 : 36, has_gpu ? 24 : 14);
    ipo_gfx3d_mesh_plane(&m_floor, 26.0, 26.0, has_gpu ? 20 : 8);
    for (uint32_t vi = 0u; vi < m_floor.vert_count; vi++) {
        m_floor.verts[vi].u *= 5.0;
        m_floor.verts[vi].v *= 5.0;
    }

    ipo_gfx3d_texture_t tex_floor;
    memset(&tex_floor, 0, sizeof(tex_floor));
    build_bench_texture_64f(&tex_floor, 256);

    ctx->ambient_sky = (ipo_rgba64f_t){ 0.24, 0.31, 0.46, 1.0 };
    ctx->ambient_ground = (ipo_rgba64f_t){ 0.06, 0.06, 0.09, 1.0 };
    ctx->enable_env_reflection = true;

    ipo_gfx3d_ensure_lights(ctx, 4u);
    ctx->lights[0].enabled = true;
    ctx->lights[0].is_point = false;
    ctx->lights[0].pos_or_dir = ipo_vec3(0.55, 0.80, 0.45);
    ctx->lights[0].color = (ipo_rgba64f_t){ 2.75, 2.52, 2.25, 1.0 };

    ctx->lights[1].enabled = true;
    ctx->lights[1].is_point = false;
    ctx->lights[1].pos_or_dir = ipo_vec3(-0.70, 0.44, -0.54);
    ctx->lights[1].color = (ipo_rgba64f_t){ 0.68, 0.96, 1.65, 1.0 };

    ctx->lights[2].enabled = true;
    ctx->lights[2].is_point = false;
    ctx->lights[2].pos_or_dir = ipo_vec3(0.72, 0.25, 0.65);
    ctx->lights[2].color = (ipo_rgba64f_t){ 0.90, 0.82, 0.72, 1.0 };

    ctx->lights[3].enabled = true;
    ctx->lights[3].is_point = true;
    ctx->lights[3].pos_or_dir = ipo_vec3(-2.5, 1.2, 4.5);
    ctx->lights[3].color = (ipo_rgba64f_t){ 1.40, 1.15, 0.85, 1.0 };

    const double floor_y = -3.28;
    float angle_deg = 0.0f;
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
                        cam_yaw += (float)dx * 0.85f;
                        cam_pitch += (float)dy * 0.65f;
                        if (cam_pitch < 2.0f) cam_pitch = 2.0f;
                        if (cam_pitch > 50.0f) cam_pitch = 50.0f;
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
            angle_deg += 46.0f * ((float)dt_ms * 0.001f);
            if (angle_deg >= 36000.0f) angle_deg -= 36000.0f;
        }

        ipo_gfx3d_clear(ctx,
                        (ipo_rgba64f_t){ 0.025, 0.034, 0.068, 1.0 },
                        (ipo_rgba64f_t){ 0.090, 0.118, 0.190, 1.0 },
                        1.0);

        double yaw_r = (double)cam_yaw * (3.1415926535 / 180.0);
        double pch_r = (double)cam_pitch * (3.1415926535 / 180.0);
        double cam_dist = 10.6;
        ipo_vec3_t eye = ipo_vec3(
            -0.10 + cam_dist * cos(pch_r) * sin(yaw_r),
            -0.25 + cam_dist * sin(pch_r),
             cam_dist * cos(pch_r) * cos(yaw_r));
        ipo_vec3_t tgt = ipo_vec3(-0.10, -0.35, 0.0);
        ctx->cam_pos = eye;
        ctx->view = ipo_mat4_look_at(eye, tgt, ipo_vec3(0.0, 1.0, 0.0));
        ctx->proj = ipo_mat4_perspective(38.0 * (3.1415926535 / 180.0),
                                         (double)ctx->width / (double)ctx->height, 0.4, 65.0);

        double ang_r = (double)angle_deg * (3.1415926535 / 180.0);
        ipo_mat4_t t_ruby = ipo_mat4_translate(-1.05, -0.80, 0.0);
        ipo_mat4_t r_ruby = ipo_mat4_rotate_z(ang_r);
        ipo_mat4_t m_ruby = ipo_mat4_mul(&t_ruby, &r_ruby);

        ipo_mat4_t t_em = ipo_mat4_translate(2.05, -0.80, 0.0);
        ipo_mat4_t r_em = ipo_mat4_rotate_z(-2.0 * ang_r - 9.0 * (3.1415926535 / 180.0));
        ipo_mat4_t m_em = ipo_mat4_mul(&t_em, &r_em);

        ipo_mat4_t t_gld = ipo_mat4_translate(-1.05, 2.30, 0.0);
        ipo_mat4_t r_gld = ipo_mat4_rotate_z(-2.0 * ang_r - 24.0 * (3.1415926535 / 180.0));
        ipo_mat4_t m_gld = ipo_mat4_mul(&t_gld, &r_gld);

        ipo_mat4_t col_rx = ipo_mat4_rotate_x(M_PI * 0.5);
        ipo_mat4_t col_tf = ipo_mat4_translate(-1.05, -0.80, 0.26);
        ipo_mat4_t col_tb = ipo_mat4_translate(-1.05, -0.80, -0.26);
        ipo_mat4_t t_col_f = ipo_mat4_mul(&col_tf, &col_rx);
        ipo_mat4_t t_col_b = ipo_mat4_mul(&col_tb, &col_rx);
        ipo_mat4_t mod_floor = ipo_mat4_translate(0.0, floor_y, 0.0);

        ipo_gfx3d_material_t mat_ruby = {
            .albedo = { 0.88, 0.14, 0.16, 1.0 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.88, .roughness = 0.16, .reflectivity = 1.08,
            .clearcoat = 0.82, .ao = 1.0, .albedo_tex = NULL
        };
        ipo_gfx3d_material_t mat_emerald = {
            .albedo = { 0.12, 0.82, 0.42, 1.0 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.88, .roughness = 0.17, .reflectivity = 1.05,
            .clearcoat = 0.80, .ao = 1.0, .albedo_tex = NULL
        };
        ipo_gfx3d_material_t mat_gold = {
            .albedo = { 1.00, 0.78, 0.28, 1.0 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.98, .roughness = 0.13, .reflectivity = 1.24,
            .clearcoat = 0.50, .ao = 1.0, .albedo_tex = NULL
        };
        ipo_gfx3d_material_t mat_platinum = {
            .albedo = { 0.86, 0.88, 0.94, 1.0 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.98, .roughness = 0.10, .reflectivity = 1.25,
            .clearcoat = 0.60, .ao = 1.0, .albedo_tex = NULL
        };
        ipo_gfx3d_material_t mat_floor = {
            .albedo = { 1.0, 1.0, 1.0, 0.64 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.12, .roughness = 0.08, .reflectivity = 0.76,
            .clearcoat = 0.90, .ao = 1.0, .albedo_tex = &tex_floor
        };

        /* 1. Planar mirror reflection of interlocking gears on reflective obsidian mat */
        ipo_gfx3d_draw_mirrored_mesh(ctx, &g_ruby,    &m_ruby,  &mat_ruby,     floor_y, 0.44);
        ipo_gfx3d_draw_mirrored_mesh(ctx, &g_emerald, &m_em,    &mat_emerald,  floor_y, 0.44);
        ipo_gfx3d_draw_mirrored_mesh(ctx, &g_gold,    &m_gld,   &mat_gold,     floor_y, 0.44);
        ipo_gfx3d_draw_mirrored_mesh(ctx, &m_collar,  &t_col_f, &mat_platinum, floor_y, 0.44);
        ipo_gfx3d_clear_depth(ctx, 1.0);

        /* 2. Translucent obsidian watchmaker mat */
        ipo_gfx3d_draw_mesh(ctx, &m_floor, &mod_floor, &mat_floor);

        /* 3. Soft 5-tap Poisson penumbra shadows */
        ipo_gfx3d_draw_shadow_projection(ctx, &g_ruby,    &m_ruby, ctx->lights[0].pos_or_dir, floor_y, 0.36);
        ipo_gfx3d_draw_shadow_projection(ctx, &g_emerald, &m_em,   ctx->lights[0].pos_or_dir, floor_y, 0.34);
        ipo_gfx3d_draw_shadow_projection(ctx, &g_gold,    &m_gld,  ctx->lights[0].pos_or_dir, floor_y, 0.30);

        /* 4. Foreground CNC chamfered metallic gears + platinum hub collars */
        ipo_gfx3d_draw_mesh(ctx, &g_ruby,    &m_ruby,  &mat_ruby);
        ipo_gfx3d_draw_mesh(ctx, &g_emerald, &m_em,    &mat_emerald);
        ipo_gfx3d_draw_mesh(ctx, &g_gold,    &m_gld,   &mat_gold);
        ipo_gfx3d_draw_mesh(ctx, &m_collar,  &t_col_f, &mat_platinum);
        ipo_gfx3d_draw_mesh(ctx, &m_collar,  &t_col_b, &mat_platinum);

        /* 5. CPU Bloom + Perceptual FXAA before HUD strip */
        ipo_gfx3d_apply_postfx(ctx, 0.15, true);

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
            serial_printf("[gl_gears] %u FPS (%dx%d)\n", (unsigned)cur_fps, ren_w, ren_h);
            fps_n = 0u;
            fps_t0 = now;
        }
        if (now - yield_t0 >= 10u) {
            yield_t0 = now;
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        }
    }

    ipo_gfx3d_tex_free(&tex_floor);
    ipo_gfx3d_mesh_free(&g_ruby);
    ipo_gfx3d_mesh_free(&g_emerald);
    ipo_gfx3d_mesh_free(&g_gold);
    ipo_gfx3d_mesh_free(&m_collar);
    ipo_gfx3d_mesh_free(&m_floor);
    ipo_gfx3d_destroy(ctx);
    if (wm_is_window_valid(win)) wm_destroy_window(win);
    return 0;
}
