#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <wm.h>
#include <syscall.h>
#include <system/timer.h>
#include <system/state.h>
#include <driver/color.h>
#include <ipo/gfx.h>
#include <ipo/gfx3d.h>
#include <ipo/gpu_client.h>
#include <ipo/audio.h>

extern int serial_printf(const char *fmt, ...);

static volatile bool s_running = true;
static volatile bool s_toggle_fs = false;
static volatile bool s_toggle_audio = false;
static bool s_audio_muted = false;

static void on_win_event(wm_window_t *win, uint32_t event, uint32_t data) {
    if (event == WM_EVENT_CLOSE) {
        s_running = false;
    } else if (event == WM_EVENT_KEY_DOWN) {
        uint8_t sc = (uint8_t)(data & 0xFFu);
        if (sc == 0x01u || sc == 0x10u) { /* Esc or Q */
            s_running = false;
        } else if (sc == 0x57u) { /* F11 */
            s_toggle_fs = true;
        } else if (sc == 0x32u) { /* M */
            s_toggle_audio = true;
        }
        (void)win;
    }
}

static void build_studio_marble_floor(ipo_gfx3d_texture_t *out, int sz) {
    out->width = sz;
    out->height = sz;
    out->texels = (ipo_rgba64f_t *)malloc((size_t)sz * (size_t)sz * sizeof(ipo_rgba64f_t));
    if (!out->texels) return;
    int tile = sz / 4;
    for (int y = 0; y < sz; y++) {
        int ty = y / tile;
        int ly = y % tile;
        double fy = (double)y / (double)sz;
        for (int x = 0; x < sz; x++) {
            int tx = x / tile;
            int lx = x % tile;
            double fx = (double)x / (double)sz;
            bool light_tile = ((tx ^ ty) & 1) == 0;
            /* Organic diagonal marble veining */
            double vein = sin(fx * 18.8495 + fy * 12.5663 + 1.4 * sin(fy * 9.4247 - fx * 6.2831));
            vein = 0.5 + 0.5 * vein;
            vein = vein * vein;

            /* Anti-aliased chamfered tile grout seam (2px border) */
            int ex = (lx < tile - 1 - lx) ? lx : (tile - 1 - lx);
            int ey = (ly < tile - 1 - ly) ? ly : (tile - 1 - ly);
            int edge = (ex < ey) ? ex : ey;
            double grout = (edge <= 1) ? 0.25 : ((edge == 2) ? 0.68 : 1.0);

            double r, g, b;
            if (light_tile) {
                r = (0.78 - 0.14 * vein) * grout;
                g = (0.81 - 0.13 * vein) * grout;
                b = (0.86 - 0.10 * vein) * grout;
            } else {
                r = (0.085 + 0.045 * vein) * grout;
                g = (0.105 + 0.055 * vein) * grout;
                b = (0.145 + 0.070 * vein) * grout;
            }
            out->texels[(size_t)y * (size_t)sz + (size_t)x] = (ipo_rgba64f_t){ r, g, b, 1.0 };
        }
    }
}

static void build_studio_cube_tex(ipo_gfx3d_texture_t *out, int sz) {
    out->width = sz;
    out->height = sz;
    out->texels = (ipo_rgba64f_t *)malloc((size_t)sz * (size_t)sz * sizeof(ipo_rgba64f_t));
    if (!out->texels) return;
    double half = (double)sz * 0.5;
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            double u = ((double)x + 0.5) / (double)sz;
            double v = ((double)y + 0.5) / (double)sz;
            double du = fabs(u - 0.5), dv = fabs(v - 0.5);
            double dmax = (du > dv) ? du : dv;
            /* Machined gunmetal border + dual-tone sapphire/amber quad panels + thin gold hairline */
            double r, g, b;
            if (dmax > 0.435) {
                double bevel = 1.0 - (dmax - 0.435) * 6.0;
                if (bevel < 0.45) bevel = 0.45;
                r = 0.16 * bevel; g = 0.19 * bevel; b = 0.25 * bevel;
            } else if (dmax > 0.415) {
                r = 0.92; g = 0.74; b = 0.34; /* Fine gold trim line */
            } else {
                bool quad = ((x < (int)half) ^ (y < (int)half)) != 0;
                double gx = fabs((double)(x % (sz / 2)) - (double)(sz / 4)) / (double)(sz / 4);
                double gy = fabs((double)(y % (sz / 2)) - (double)(sz / 4)) / (double)(sz / 4);
                double sheen = 1.0 - 0.22 * (gx * gx + gy * gy);
                if (quad) {
                    r = 0.06 * sheen; g = 0.48 * sheen; b = 0.88 * sheen;
                } else {
                    r = 0.86 * sheen; g = 0.32 * sheen; b = 0.08 * sheen;
                }
            }
            out->texels[(size_t)y * (size_t)sz + (size_t)x] = (ipo_rgba64f_t){ r, g, b, 1.0 };
        }
    }
}

static void sdk_calc_render_size(wm_window_t *win, bool has_gpu, int *out_w, int *out_h) {
    ipo_gfx_caps_t c;
    memset(&c, 0, sizeof(c));
    if (ipo_gfx_get_caps(&c) == 0 && c.active_hw_bpp >= 15u) {
        if (win && win->fullscreen && c.active_width >= 320u && c.active_height >= 200u) {
            *out_w = (int)c.active_width;
            *out_h = (int)c.active_height;
        } else {
            int px = 0, py = 0, pw = 0, ph = 0;
            ipo_gfx_wm_window_phys_rect(win, &px, &py, &pw, &ph);
            *out_w = (pw > 64) ? pw : 320;
            *out_h = (ph > 64) ? ph : 220;
        }
        if (!has_gpu && *out_w > 560) {
            *out_h = (*out_h * 560) / (*out_w);
            *out_w = 560;
            if (*out_h < 180) *out_h = 180;
        }
        return;
    }
    *out_w = 320;
    *out_h = 220;
}

static void fill_chord_pcm16(int16_t *out, int frames, uint32_t *phase_a, uint32_t *phase_b,
                             uint32_t *phase_c, int sample_rate, uint32_t step_idx) {
    /* Pentatonic chord frequencies (Hz) cycling smoothly across step_idx */
    static const uint32_t root_hz[4] = { 220u, 261u, 293u, 329u };
    uint32_t f0 = root_hz[(step_idx / 40u) & 3u];
    uint32_t f1 = (f0 * 3u) / 2u;
    uint32_t f2 = f0 * 2u;
    uint32_t inc0 = (uint32_t)(((uint64_t)f0 << 16) / (uint32_t)sample_rate);
    uint32_t inc1 = (uint32_t)(((uint64_t)f1 << 16) / (uint32_t)sample_rate);
    uint32_t inc2 = (uint32_t)(((uint64_t)f2 << 16) / (uint32_t)sample_rate);

    for (int i = 0; i < frames; i++) {
        /* Clean triangle/sine blend using exact integer wave table free of FPU overhead */
        uint32_t p0 = (*phase_a >> 8) & 255u;
        uint32_t p1 = (*phase_b >> 8) & 255u;
        uint32_t p2 = (*phase_c >> 8) & 255u;
        int32_t t0 = (p0 < 128u) ? ((int32_t)p0 * 4 - 256) : (256 - ((int32_t)p0 - 128) * 4);
        int32_t t1 = (p1 < 128u) ? ((int32_t)p1 * 4 - 256) : (256 - ((int32_t)p1 - 128) * 4);
        int32_t t2 = (p2 < 128u) ? ((int32_t)p2 * 4 - 256) : (256 - ((int32_t)p2 - 128) * 4);
        int32_t L = (t0 * 14 + t1 * 9 + t2 * 5);
        int32_t R = (t0 * 14 + t1 * 5 + t2 * 9);
        out[i * 2 + 0] = (int16_t)(L > 32767 ? 32767 : (L < -32768 ? -32768 : L));
        out[i * 2 + 1] = (int16_t)(R > 32767 ? 32767 : (R < -32768 ? -32768 : R));
        *phase_a += inc0;
        *phase_b += inc1;
        *phase_c += inc2;
    }
}

static void fill_pulse_pcm16(int16_t *out, int frames, uint32_t *phase_p, int sample_rate, uint32_t *sample_ctr) {
    uint32_t inc = (uint32_t)((110ULL << 16) / (uint32_t)sample_rate);
    for (int i = 0; i < frames; i++) {
        uint32_t p = (*phase_p >> 8) & 255u;
        int32_t tri = (p < 128u) ? ((int32_t)p * 4 - 256) : (256 - ((int32_t)p - 128) * 4);
        uint32_t beat = (*sample_ctr % (uint32_t)(sample_rate / 2));
        int32_t env = (beat < (uint32_t)(sample_rate / 6))
                    ? (int32_t)(16 - (beat * 16u) / (uint32_t)(sample_rate / 6))
                    : 0;
        int32_t s = (tri * env * 2);
        out[i] = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
        *phase_p += inc;
        (*sample_ctr)++;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    ipo_gfx_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    ipo_gfx_get_caps(&caps);

    int mode_cnt = 0;
    ipo_gfx_mode_info_t mi;
    while (mode_cnt < 64 && ipo_gfx_enum_modes((uint32_t)mode_cnt, &mi) == 0) {
        mode_cnt++;
    }

    uint32_t audio_ver = ipo_audio_api_version();
    int dev_cnt = ipo_audio_get_device_count();
    ipo_audio_device_info_t ainfo;
    memset(&ainfo, 0, sizeof(ainfo));
    ainfo.struct_size = (uint32_t)sizeof(ainfo);
    if (dev_cnt > 0) {
        ipo_audio_get_device_info(0u, &ainfo);
    }

    serial_printf("[sdk_3d_audio_demo] gfx_api=0x%x bga=%u hw_bpp=%u wc=%u modes=%d max_fmt=%u (%ubpp)\n",
                  caps.api_version, caps.hw_bga_available, caps.active_hw_bpp,
                  caps.lfb_wc_enabled, mode_cnt, caps.sw_pipeline_max_fmt_id,
                  caps.sw_pipeline_max_bpp);
    serial_printf("[sdk_3d_audio_demo] audio_api=0x%x devs=%d name='%s' native=%uHz arb_fp=%u\n",
                  audio_ver, dev_cnt, ainfo.name[0] ? ainfo.name : "none",
                  ainfo.native_sample_rate, ainfo.fp_arbitrary_supported);

    /* Open 2 concurrent public audio streams with non-standard Fixed-Point formats */
    ipo_audio_handle_t h_pad = 0u, h_bass = 0u;
    bool pad_ok = false, bass_ok = false;

    if (dev_cnt > 0) {
        ipo_audio_open_params_t p0;
        memset(&p0, 0, sizeof(p0));
        p0.struct_size = (uint32_t)sizeof(p0);
        p0.device_id = 0u;
        p0.direction = IPO_AUDIO_DIR_PLAYBACK;
        p0.format = AUDIO_FORMAT_S16;
        p0.channels = 2;
        p0.sample_rate = 44100u;
        p0.buffer_frames = 16384u;
        p0.use_fp_config = 1u;
        p0.fp_config.pos_format.total_bits = 32u;
        p0.fp_config.pos_format.int_bits = 18u;
        p0.fp_config.pos_format.frac_bits = 13u;
        p0.fp_config.pos_format.is_signed = 1u;
        p0.fp_config.pos_format.round_mode = AUDIO_FP_ROUND_HALF_UP;
        p0.fp_config.pos_format.overflow_mode = AUDIO_FP_OVERFLOW_SATURATE;
        p0.fp_config.mix_format.total_bits = 32u;
        p0.fp_config.mix_format.int_bits = 10u;
        p0.fp_config.mix_format.frac_bits = 21u;
        p0.fp_config.mix_format.is_signed = 1u;
        p0.fp_config.mix_format.round_mode = AUDIO_FP_ROUND_HALF_UP;
        p0.fp_config.mix_format.overflow_mode = AUDIO_FP_OVERFLOW_SATURATE;
        if (ipo_audio_open(&p0, &h_pad) == IPO_AUDIO_OK) {
            pad_ok = true;
            ipo_audio_set_volume(h_pad, 0.35f);
        }

        ipo_audio_open_params_t p1;
        memset(&p1, 0, sizeof(p1));
        p1.struct_size = (uint32_t)sizeof(p1);
        p1.device_id = 0u;
        p1.direction = IPO_AUDIO_DIR_PLAYBACK;
        p1.format = AUDIO_FORMAT_S16;
        p1.channels = 1;
        p1.sample_rate = 22050u;
        p1.buffer_frames = 8192u;
        p1.use_fp_config = 1u;
        p1.fp_config.pos_format.total_bits = 36u;
        p1.fp_config.pos_format.int_bits = 20u;
        p1.fp_config.pos_format.frac_bits = 16u;
        p1.fp_config.pos_format.is_signed = 0u;
        p1.fp_config.pos_format.round_mode = AUDIO_FP_ROUND_TRUNC;
        p1.fp_config.pos_format.overflow_mode = AUDIO_FP_OVERFLOW_SATURATE;
        p1.fp_config.mix_format.total_bits = 24u;
        p1.fp_config.mix_format.int_bits = 11u;
        p1.fp_config.mix_format.frac_bits = 12u;
        p1.fp_config.mix_format.is_signed = 1u;
        p1.fp_config.mix_format.round_mode = AUDIO_FP_ROUND_HALF_UP;
        p1.fp_config.mix_format.overflow_mode = AUDIO_FP_OVERFLOW_SATURATE;
        if (ipo_audio_open(&p1, &h_bass) == IPO_AUDIO_OK) {
            bass_ok = true;
            ipo_audio_set_volume(h_bass, 0.30f);
        }
    }

    int sw = 320, sh = 200;
    wm_get_screen_bounds(&sw, &sh);
    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "IPO SDK 3D RGBA64F + Audio";
    opt.w = (uint16_t)(sw >= 320 ? 280 : 220);
    opt.h = (uint16_t)(sh >= 200 ? 180 : 150);
    opt.x = 18;
    opt.y = 14;
    opt.decor_style = WM_DECOR_DEFAULT;

    (void)ipo_gfx_ensure_hd_mode(1280u, 720u);
    wm_window_t *win = wm_create_window(&opt);
    if (!win) {
        if (pad_ok) ipo_audio_close(h_pad);
        if (bass_ok) ipo_audio_close(h_bass);
        return 1;
    }

    bool has_gpu = ipo_gpu_3d_available();
    int ren_w = 320, ren_h = 220;
    sdk_calc_render_size(win, has_gpu, &ren_w, &ren_h);
    ipo_gfx3d_ctx_t *ctx = ipo_gfx3d_create(ren_w, ren_h);
    if (!ctx) {
        wm_destroy_window(win);
        if (pad_ok) ipo_audio_close(h_pad);
        if (bass_ok) ipo_audio_close(h_bass);
        return 1;
    }

    ipo_gfx3d_mesh_t m_floor, m_torus, m_sphere, m_cube;
    memset(&m_floor, 0, sizeof(m_floor));
    memset(&m_torus, 0, sizeof(m_torus));
    memset(&m_sphere, 0, sizeof(m_sphere));
    memset(&m_cube, 0, sizeof(m_cube));
    ipo_gfx3d_mesh_plane(&m_floor, 16.0, 16.0, has_gpu ? 20 : 8);
    for (uint32_t vi = 0u; vi < m_floor.vert_count; vi++) {
        m_floor.verts[vi].u *= 3.2;
        m_floor.verts[vi].v *= 3.2;
    }
    ipo_gfx3d_mesh_torus(&m_torus, 0.88, 0.29, has_gpu ? 84 : 40, has_gpu ? 44 : 20);
    ipo_gfx3d_mesh_sphere(&m_sphere, 0.56, has_gpu ? 44 : 22, has_gpu ? 68 : 32);
    ipo_gfx3d_mesh_rounded_cube(&m_cube, 0.52, 0.11, has_gpu ? 16 : 10);

    ipo_gfx3d_texture_t tex_floor, tex_cube;
    memset(&tex_floor, 0, sizeof(tex_floor));
    memset(&tex_cube, 0, sizeof(tex_cube));
    build_studio_marble_floor(&tex_floor, 256);
    build_studio_cube_tex(&tex_cube, 256);

    ctx->ambient_sky = (ipo_rgba64f_t){ 0.24, 0.32, 0.50, 1.0 };
    ctx->ambient_ground = (ipo_rgba64f_t){ 0.06, 0.05, 0.08, 1.0 };
    ctx->enable_env_reflection = true;

    ipo_gfx3d_ensure_lights(ctx, 4u);
    ctx->lights[0].enabled = true;
    ctx->lights[0].is_point = false;
    ctx->lights[0].pos_or_dir = ipo_vec3(0.58, 0.80, 0.42);
    ctx->lights[0].color = (ipo_rgba64f_t){ 2.15, 1.98, 1.75, 1.0 };

    ctx->lights[1].enabled = true;
    ctx->lights[1].is_point = false;
    ctx->lights[1].pos_or_dir = ipo_vec3(-0.70, 0.48, -0.55);
    ctx->lights[1].color = (ipo_rgba64f_t){ 0.55, 0.82, 1.45, 1.0 };

    ctx->lights[2].enabled = true;
    ctx->lights[2].is_point = true;
    ctx->lights[2].pos_or_dir = ipo_vec3(-1.4, 1.15, 1.65);
    ctx->lights[2].color = (ipo_rgba64f_t){ 1.55, 0.72, 0.38, 1.0 };

    ctx->lights[3].enabled = true;
    ctx->lights[3].is_point = false;
    ctx->lights[3].pos_or_dir = ipo_vec3(0.62, 0.35, -0.70);
    ctx->lights[3].color = (ipo_rgba64f_t){ 0.65, 0.72, 0.88, 1.0 };

    uint32_t ph_a = 0, ph_b = 0, ph_c = 0, ph_p = 0, step_idx = 0, sample_ctr = 0;
    int16_t pcm_pad[1024];   /* 512 stereo frames */
    int16_t pcm_bass[512];   /* 512 mono frames */

    uint32_t t_start = timer_millis();
    uint32_t fps_t0 = t_start;
    uint32_t fps_frames = 0;
    int cur_fps = 0;
    uint32_t last_yield_ms = t_start;
    uint16_t last_ww = win->w, last_wh = win->h;
    bool last_fs = win->fullscreen;

    while (s_running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        wm_event_t ev;
        while (wm_poll_event(win, &ev)) {
            on_win_event(win, ev.type, ev.data);
        }
        if (s_toggle_fs) {
            s_toggle_fs = false;
            wm_set_fullscreen(win, !win->fullscreen);
        }
        if (win->w != last_ww || win->h != last_wh || win->fullscreen != last_fs) {
            last_ww = win->w;
            last_wh = win->h;
            last_fs = win->fullscreen;
            int nw = ren_w, nh = ren_h;
            sdk_calc_render_size(win, has_gpu, &nw, &nh);
            if ((nw != ctx->width || nh != ctx->height) && ipo_gfx3d_resize(ctx, nw, nh)) {
                ren_w = nw;
                ren_h = nh;
            }
        }
        if (s_toggle_audio) {
            s_toggle_audio = false;
            s_audio_muted = !s_audio_muted;
            if (pad_ok) ipo_audio_pause(h_pad, s_audio_muted);
            if (bass_ok) ipo_audio_pause(h_bass, s_audio_muted);
        }
        if (!s_running) break;

        /* Top up both public fixed-point audio streams based on real hardware clock */
        ipo_audio_clock_t clk_pad, clk_bass;
        memset(&clk_pad, 0, sizeof(clk_pad));
        memset(&clk_bass, 0, sizeof(clk_bass));
        clk_pad.struct_size = (uint32_t)sizeof(clk_pad);
        clk_bass.struct_size = (uint32_t)sizeof(clk_bass);
        if (pad_ok && !s_audio_muted && ipo_audio_get_clock(h_pad, &clk_pad) == 0) {
            while (clk_pad.queued_frames < 8192u) {
                fill_chord_pcm16(pcm_pad, 512, &ph_a, &ph_b, &ph_c, 44100, step_idx++);
                if (ipo_audio_write(h_pad, pcm_pad, sizeof(pcm_pad)) <= 0) break;
                clk_pad.queued_frames += 512u;
            }
        }
        if (bass_ok && !s_audio_muted && ipo_audio_get_clock(h_bass, &clk_bass) == 0) {
            while (clk_bass.queued_frames < 4096u) {
                fill_pulse_pcm16(pcm_bass, 512, &ph_p, 22050, &sample_ctr);
                if (ipo_audio_write(h_bass, pcm_bass, sizeof(pcm_bass)) <= 0) break;
                clk_bass.queued_frames += 512u;
            }
        }

        uint32_t now = timer_millis();
        double t = (double)(now - t_start) * 0.001;

        ipo_gfx3d_clear(ctx,
                        (ipo_rgba64f_t){ 0.04, 0.07, 0.14, 1.0 },
                        (ipo_rgba64f_t){ 0.14, 0.19, 0.28, 1.0 },
                        1.0);

        ipo_vec3_t eye = ipo_vec3(0.0, 1.65, 4.2);
        ipo_vec3_t target = ipo_vec3(0.0, 0.10, 0.0);
        ctx->cam_pos = eye;
        ctx->view = ipo_mat4_look_at(eye, target, ipo_vec3(0.0, 1.0, 0.0));
        ctx->proj = ipo_mat4_perspective(45.0 * (3.1415926535 / 180.0),
                                         (double)ctx->width / (double)ctx->height, 0.2, 40.0);

        /* Prepare scene transforms & PBR materials */
        ipo_mat4_t mod_floor = ipo_mat4_translate(0.0, -1.0, 0.0);
        ipo_gfx3d_material_t mat_floor = {
            .albedo = { 1.0, 1.0, 1.0, 0.64 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.08, .roughness = 0.08, .reflectivity = 0.72,
            .clearcoat = 0.90, .ao = 1.0, .albedo_tex = &tex_floor
        };

        ipo_mat4_t r_y = ipo_mat4_rotate_y(t * 0.9);
        ipo_mat4_t r_x = ipo_mat4_rotate_x(t * 0.65);
        ipo_mat4_t r_torus = ipo_mat4_mul(&r_y, &r_x);
        ipo_mat4_t t_torus = ipo_mat4_translate(0.0, 0.28, 0.0);
        ipo_mat4_t mod_torus = ipo_mat4_mul(&t_torus, &r_torus);

        double orb_x = cos(t * 1.15) * 1.65;
        double orb_z = sin(t * 1.15) * 1.45;
        ipo_mat4_t mod_sphere = ipo_mat4_translate(orb_x, 0.05 + 0.15 * sin(t * 2.3), orb_z);

        ipo_mat4_t t_cube = ipo_mat4_translate(-1.35, -0.35, -0.75);
        ipo_mat4_t r_cube = ipo_mat4_rotate_y(-t * 1.1);
        ipo_mat4_t mod_cube = ipo_mat4_mul(&t_cube, &r_cube);

        ipo_gfx3d_material_t mat_gold = {
            .albedo = { 1.00, 0.79, 0.30, 1.0 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.98, .roughness = 0.13, .reflectivity = 1.12,
            .clearcoat = 0.45, .ao = 1.0, .albedo_tex = NULL
        };
        ipo_gfx3d_material_t mat_chrome = {
            .albedo = { 0.48, 0.93, 1.00, 1.0 },
            .emissive = { 0.0, 0.02, 0.03, 1.0 },
            .metallic = 0.96, .roughness = 0.08, .reflectivity = 1.22,
            .clearcoat = 0.75, .ao = 1.0, .albedo_tex = NULL
        };
        ipo_gfx3d_material_t mat_cube = {
            .albedo = { 1.0, 1.0, 1.0, 1.0 },
            .emissive = { 0.0, 0.0, 0.0, 1.0 },
            .metallic = 0.28, .roughness = 0.18, .reflectivity = 0.55,
            .clearcoat = 0.78, .ao = 1.0, .albedo_tex = &tex_cube
        };

        /* 1. Planar mirror reflection pass below y = -1.0 before floor composite */
        ipo_gfx3d_draw_mirrored_mesh(ctx, &m_torus,  &mod_torus,  &mat_gold,   -1.0, 0.42);
        ipo_gfx3d_draw_mirrored_mesh(ctx, &m_sphere, &mod_sphere, &mat_chrome, -1.0, 0.42);
        ipo_gfx3d_draw_mirrored_mesh(ctx, &m_cube,   &mod_cube,   &mat_cube,   -1.0, 0.42);
        ipo_gfx3d_clear_depth(ctx, 1.0);

        /* 2. Polished translucent reflective studio marble floor at y = -1.0 */
        ipo_gfx3d_draw_mesh(ctx, &m_floor, &mod_floor, &mat_floor);

        /* 3. Soft 5-tap Poisson penumbra shadows onto y = -1.0 */
        ipo_gfx3d_draw_shadow_projection(ctx, &m_torus,  &mod_torus,  ctx->lights[0].pos_or_dir, -1.0, 0.46);
        ipo_gfx3d_draw_shadow_projection(ctx, &m_sphere, &mod_sphere, ctx->lights[0].pos_or_dir, -1.0, 0.44);
        ipo_gfx3d_draw_shadow_projection(ctx, &m_cube,   &mod_cube,   ctx->lights[0].pos_or_dir, -1.0, 0.44);

        /* 4. Foreground PBR meshes */
        ipo_gfx3d_draw_mesh(ctx, &m_torus,  &mod_torus,  &mat_gold);
        ipo_gfx3d_draw_mesh(ctx, &m_sphere, &mod_sphere, &mat_chrome);
        ipo_gfx3d_draw_mesh(ctx, &m_cube,   &mod_cube,   &mat_cube);

        /* 5. CPU Post-FX (Bloom + Perceptual FXAA) before crisp HUD overlay */
        ipo_gfx3d_apply_postfx(ctx, 0.16, true);

        /* 6. Linear RGBA64F 2D HUD & Audio Visualizer on the exact same 3D scene */
        ipo_gfx3d_draw_rect2d(ctx, 0, 0, ctx->width, 22, (ipo_rgba64f_t){ 0.015, 0.02, 0.04, 0.82 });
        ipo_gfx3d_draw_line2d(ctx, 0, 22, ctx->width - 1, 22, (ipo_rgba64f_t){ 0.25, 0.70, 0.95, 0.85 });
        const char *mode_s = has_gpu ? "VirGL MSAA" : "RGBA64F";
        int hlen = snprintf(NULL, 0, "%s | %d FPS | %dx%d | Tris:%u | Aud:%ums",
                            mode_s, cur_fps, ctx->width, ctx->height,
                            (unsigned)ctx->tris_drawn, (unsigned)clk_pad.played_ms);
        if (hlen > 0) {
            char *hud = (char *)malloc((size_t)hlen + 1u);
            if (hud) {
                snprintf(hud, (size_t)hlen + 1u, "%s | %d FPS | %dx%d | Tris:%u | Aud:%ums",
                         mode_s, cur_fps, ctx->width, ctx->height,
                         (unsigned)ctx->tris_drawn, (unsigned)clk_pad.played_ms);
                ipo_gfx3d_draw_text2d(ctx, 6, 7, hud, (ipo_rgba64f_t){ 0.95, 0.98, 1.0, 1.0 }, 1);
                free(hud);
            }
        }

        /* Live audio bar meter inside dark bottom HUD strip */
        ipo_gfx3d_draw_rect2d(ctx, 0, ctx->height - 16, ctx->width, 16, (ipo_rgba64f_t){ 0.015, 0.02, 0.04, 0.86 });
        for (int b = 0; b < 16; b++) {
            int bh = 4 + (int)(((uint32_t)(sample_ctr + b * 313u) * 3u) % 10u);
            if (s_audio_muted || !pad_ok) bh = 2;
            ipo_gfx3d_draw_rect2d(ctx, 4 + b * 5, ctx->height - 3 - bh, 3, bh,
                                  (ipo_rgba64f_t){ 0.20, 0.88, 0.45, 0.92 });
        }
        const char *bot_txt = s_audio_muted
            ? "M:Unmute F11:FS Esc:Quit"
            : ((ctx->width >= 440) ? "2x ArbFP Audio + 3D Studio (M:Mute F11:FS)" : "Audio+3D (M/F11)");
        ipo_gfx3d_draw_text2d(ctx, 90, ctx->height - 12, bot_txt,
                              (ipo_rgba64f_t){ 0.90, 0.94, 1.0, 1.0 }, 1);

        while (wm_poll_event(win, &ev)) {
            on_win_event(win, ev.type, ev.data);
        }
        if (!s_running || !wm_is_window_valid(win)) break;
        ipo_gfx3d_present(ctx, win, IPO_TONEMAP_ACES, 1.0);
        wm_compositor_tick();
        if (pad_ok) ipo_audio_get_clock(h_pad, &clk_pad);
        if (bass_ok) ipo_audio_get_clock(h_bass, &clk_bass);

        fps_frames++;
        now = timer_millis();
        uint32_t dt = now - fps_t0;
        if (dt >= 1000u) {
            cur_fps = (int)((fps_frames * 1000u + dt / 2u) / dt);
            uint32_t ms_pf = fps_frames ? (dt / fps_frames) : dt;
            ipo_gfx_stats_t gs;
            memset(&gs, 0, sizeof(gs));
            ipo_gfx_get_stats(&gs);
            serial_printf("[sdk_3d_audio_demo] %d FPS (%ums/f) tris=%u pad_played=%ums ur=%u bass_ur=%u gfx_pres=%u\n",
                          cur_fps, (unsigned)ms_pf, (unsigned)ctx->tris_drawn, (unsigned)clk_pad.played_ms,
                          (unsigned)clk_pad.underruns, (unsigned)clk_bass.underruns,
                          (unsigned)gs.frames_presented);
            int tlen = snprintf(NULL, 0, "SDK 3D+Audio [%d FPS]", cur_fps);
            if (tlen > 0) {
                char *tbuf = (char *)malloc((size_t)tlen + 1u);
                if (tbuf) {
                    snprintf(tbuf, (size_t)tlen + 1u, "SDK 3D+Audio [%d FPS]", cur_fps);
                    wm_set_title(win, tbuf);
                    free(tbuf);
                }
            }
            fps_frames = 0;
            fps_t0 = now;
        }
        if (now - last_yield_ms >= 10u) {
            last_yield_ms = now;
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
        }
    }

    if (pad_ok) ipo_audio_close(h_pad);
    if (bass_ok) ipo_audio_close(h_bass);
    ipo_gfx3d_tex_free(&tex_floor);
    ipo_gfx3d_tex_free(&tex_cube);
    ipo_gfx3d_mesh_free(&m_floor);
    ipo_gfx3d_mesh_free(&m_torus);
    ipo_gfx3d_mesh_free(&m_sphere);
    ipo_gfx3d_mesh_free(&m_cube);
    ipo_gfx3d_destroy(ctx);
    if (wm_is_window_valid(win)) wm_destroy_window(win);
    return 0;
}
