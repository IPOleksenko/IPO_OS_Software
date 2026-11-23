#include "quakedef.h"
#include "d_local.h"

#include <stdlib.h>
#include <string.h>

#include <driver/input/keyboard.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>

#include "common/ipo_hd_present.h"

viddef_t vid;

unsigned short d_8to16table[256];
unsigned d_8to24table[256];
void (*vid_menudrawfn)(void) = NULL;
void (*vid_menukeyfn)(int key) = NULL;

static int s_vid_w = 400;
static int s_vid_h = 300;
static byte *s_vid_buffer = NULL;
static short *s_zbuffer = NULL;
static byte *s_surfcache = NULL;
static int s_surfcache_size = 0;

wm_window_t *g_quake_wm_win = NULL;
bool g_quake_direct_gfx = false;
static ipo_hd_view_t s_hd;

static uint8_t s_cur_pal[256][3];
static uint8_t s_hw_pal[256][3];
static bool s_hw_pal_valid = false;
static uint8_t s_wm_palette[248][3];
static bool s_wm_pal_ready = false;
static uint8_t s_wm_lut[256];
static bool s_wm_lut_valid = false;
static uint8_t s_last_lut_pal[256][3];
static uint32_t s_pal32[256];

static void init_wm_palette_table(void) {
    if (s_wm_pal_ready) return;
    static const uint8_t cga[16][3] = {
        {  0,   0,   0}, {  0,   0, 170}, {  0, 170,   0}, {  0, 170, 170},
        {170,   0,   0}, {170,   0, 170}, {170,  85,   0}, {170, 170, 170},
        { 85,  85,  85}, { 85,  85, 255}, { 85, 255,  85}, { 85, 255, 255},
        {255,  85,  85}, {255,  85, 255}, {255, 255,  85}, {255, 255, 255}
    };
    for (int i = 0; i < 16; i++) {
        s_wm_palette[i][0] = cga[i][0];
        s_wm_palette[i][1] = cga[i][1];
        s_wm_palette[i][2] = cga[i][2];
    }
    for (int i = 0; i < 16; i++) {
        uint8_t v = (uint8_t)((i * 255) / 15);
        s_wm_palette[16 + i][0] = v;
        s_wm_palette[16 + i][1] = v;
        s_wm_palette[16 + i][2] = v;
    }
    int idx = 32;
    for (int r = 0; r < 6; r++) {
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                s_wm_palette[idx][0] = (uint8_t)(r * 51);
                s_wm_palette[idx][1] = (uint8_t)(g * 51);
                s_wm_palette[idx][2] = (uint8_t)(b * 51);
                idx++;
            }
        }
    }
    s_wm_pal_ready = true;
}

static void restore_wm_dac_palette(void) {
    init_wm_palette_table();
    for (int i = 0; i < 248; i++) {
        vga_gfx_set_palette((uint8_t)i,
                            (uint8_t)(s_wm_palette[i][0] >> 2),
                            (uint8_t)(s_wm_palette[i][1] >> 2),
                            (uint8_t)(s_wm_palette[i][2] >> 2));
    }
    s_hw_pal_valid = false;
}

static uint8_t nearest_wm_color(uint8_t r, uint8_t g, uint8_t b) {
    init_wm_palette_table();
    int best = 0;
    uint32_t best_d = 0xFFFFFFFFu;
    int luma_src = (int)r * 30 + (int)g * 59 + (int)b * 11;
    int drg_src = (int)r - (int)g;
    int dgb_src = (int)g - (int)b;
    for (int i = 0; i < 248; i++) {
        int pr = (int)s_wm_palette[i][0];
        int pg = (int)s_wm_palette[i][1];
        int pb = (int)s_wm_palette[i][2];
        int dr = (int)r - pr;
        int dg = (int)g - pg;
        int db = (int)b - pb;
        int luma_dst = pr * 30 + pg * 59 + pb * 11;
        int dl = (luma_src - luma_dst) / 50;
        int d_rg = drg_src - (pr - pg);
        int d_gb = dgb_src - (pg - pb);
        uint32_t d = (uint32_t)(dr * dr * 2 + dg * dg * 3 + db * db +
                                dl * dl * 2 + d_rg * d_rg + d_gb * d_gb);
        if (d == 0u) return (uint8_t)i;
        if (d < best_d) { best_d = d; best = i; }
    }
    return (uint8_t)best;
}

static void apply_hw_palette(const uint8_t pal[256][3]) {
    for (int i = 0; i < 256; i++) {
        if (!s_hw_pal_valid ||
            s_hw_pal[i][0] != pal[i][0] ||
            s_hw_pal[i][1] != pal[i][1] ||
            s_hw_pal[i][2] != pal[i][2]) {
            s_hw_pal[i][0] = pal[i][0];
            s_hw_pal[i][1] = pal[i][1];
            s_hw_pal[i][2] = pal[i][2];
            vga_gfx_set_palette((uint8_t)i,
                                (uint8_t)(pal[i][0] >> 2),
                                (uint8_t)(pal[i][1] >> 2),
                                (uint8_t)(pal[i][2] >> 2));
        }
    }
    s_hw_pal_valid = true;
}

void VID_SetPalette(unsigned char *palette) {
    if (!palette) return;
    for (int i = 0; i < 256; i++) {
        uint8_t r = palette[i * 3 + 0];
        uint8_t g = palette[i * 3 + 1];
        uint8_t b = palette[i * 3 + 2];
        s_cur_pal[i][0] = r;
        s_cur_pal[i][1] = g;
        s_cur_pal[i][2] = b;
        s_pal32[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }

    if (g_quake_direct_gfx) {
        apply_hw_palette(s_cur_pal);
    }
}

void VID_ShiftPalette(unsigned char *palette) {
    VID_SetPalette(palette);
}

void VID_Init(unsigned char *palette) {
    int p;
    if ((p = COM_CheckParm("-width")) != 0 && p + 1 < com_argc) {
        int w = Q_atoi(com_argv[p + 1]);
        if (w >= 320 && w <= MAXWIDTH) s_vid_w = w;
    }
    if ((p = COM_CheckParm("-height")) != 0 && p + 1 < com_argc) {
        int h = Q_atoi(com_argv[p + 1]);
        if (h >= 200 && h <= MAXHEIGHT) s_vid_h = h;
    }

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Quake";
    opt.bg_color = 0;
    opt.fullscreen = false;
    opt.w = 320;
    opt.h = 200;
    opt.x = 0;
    opt.y = 0;

    g_quake_wm_win = wm_create_window(&opt);
    if (g_quake_wm_win) {
        wm_set_fullscreen(g_quake_wm_win, true);
        wm_set_cursor_visible(g_quake_wm_win, false);
        wm_compositor_tick();
    } else {
        vga_set_mode_13h();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        g_quake_direct_gfx = true;
    }

    ipo_hd_setup(&s_hd, g_quake_wm_win, s_vid_w, s_vid_h, true);
    serial_printf("[quake] video ready %dx%d wm=%p direct=%d scr=%dx%d pres=(%d,%d %dx%d) k=%d\n",
                  s_vid_w, s_vid_h, (void *)g_quake_wm_win, g_quake_direct_gfx ? 1 : 0,
                  s_hd.scr_w, s_hd.scr_h,
                  s_hd.pres_x, s_hd.pres_y, s_hd.pres_w, s_hd.pres_h, s_hd.scale_k);

    size_t pix = (size_t)s_vid_w * (size_t)s_vid_h;
    s_vid_buffer = (byte *)calloc(pix, 1u);
    s_zbuffer = (short *)calloc(pix, sizeof(short));
    s_surfcache_size = 4 * 1024 * 1024;
    s_surfcache = (byte *)malloc((size_t)s_surfcache_size);
    if (!s_vid_buffer || !s_zbuffer || !s_surfcache) {
        Sys_Error("VID_Init: out of memory");
    }

    vid.maxwarpwidth = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    vid.width = vid.conwidth = (unsigned)s_vid_w;
    vid.height = vid.conheight = (unsigned)s_vid_h;
    vid.aspect = ((float)s_vid_h / (float)s_vid_w) * (320.0f / 240.0f);
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));
    vid.buffer = vid.conbuffer = s_vid_buffer;
    vid.rowbytes = vid.conrowbytes = (unsigned)s_vid_w;
    vid.direct = NULL;

    d_pzbuffer = s_zbuffer;
    D_InitCaches(s_surfcache, s_surfcache_size);

    VID_SetPalette(palette);
}

void VID_Shutdown(void) {
    if (g_quake_wm_win && wm_is_window_valid(g_quake_wm_win)) {
        restore_wm_dac_palette();
        wm_set_cursor_visible(g_quake_wm_win, true);
        wm_destroy_window(g_quake_wm_win);
        g_quake_wm_win = NULL;
    } else if (g_quake_direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
        g_quake_direct_gfx = false;
    }
    ipo_hd_free(&s_hd);
    free(s_vid_buffer); s_vid_buffer = NULL;
    free(s_zbuffer);    s_zbuffer = NULL;
    free(s_surfcache);  s_surfcache = NULL;
}

void VID_Update(vrect_t *rects) {
    (void)rects;
    if (!s_vid_buffer) return;
    static bool s_first_frame = false;
    if (!s_first_frame) {
        s_first_frame = true;
        serial_printf("[quake] first VID_Update %dx%d\n", s_vid_w, s_vid_h);
    }

    if (g_quake_direct_gfx) {
        if (!ipo_hd_present_pal32(&s_hd, NULL, true, s_vid_buffer, s_vid_w, s_vid_h, s_vid_w, s_pal32)) {
            apply_hw_palette(s_cur_pal);
            ipo_hd_present(&s_hd, NULL, true, s_vid_buffer, s_vid_w, s_vid_h, s_vid_w);
        }
    } else if (g_quake_wm_win && wm_is_window_valid(g_quake_wm_win)) {
        if (!ipo_hd_present_pal32(&s_hd, g_quake_wm_win, false, s_vid_buffer, s_vid_w, s_vid_h, s_vid_w, s_pal32)) {
            static uint8_t *mapped = NULL;
            static size_t mapped_cap = 0;
            size_t need = (size_t)s_vid_w * (size_t)s_vid_h;
            if (need > mapped_cap) {
                uint8_t *new_mapped = (uint8_t *)realloc(mapped, need);
                if (new_mapped) {
                    mapped = new_mapped;
                    mapped_cap = need;
                }
            }
            if (mapped && mapped_cap >= need) {
                for (int i = 0; i < 256; i++) {
                    if (!s_wm_lut_valid ||
                        s_last_lut_pal[i][0] != s_cur_pal[i][0] ||
                        s_last_lut_pal[i][1] != s_cur_pal[i][1] ||
                        s_last_lut_pal[i][2] != s_cur_pal[i][2]) {
                        s_last_lut_pal[i][0] = s_cur_pal[i][0];
                        s_last_lut_pal[i][1] = s_cur_pal[i][1];
                        s_last_lut_pal[i][2] = s_cur_pal[i][2];
                        s_wm_lut[i] = nearest_wm_color(s_cur_pal[i][0], s_cur_pal[i][1], s_cur_pal[i][2]);
                    }
                }
                s_wm_lut_valid = true;
                for (size_t k = 0; k < need; k++) mapped[k] = s_wm_lut[s_vid_buffer[k]];
                ipo_hd_present(&s_hd, g_quake_wm_win, false, mapped, s_vid_w, s_vid_h, s_vid_w);
            }
        }
    }
    {
        static uint32_t s_fps_t0 = 0, s_fps_n = 0;
        uint32_t now = (uint32_t)timer_millis();
        if (s_fps_t0 == 0u) s_fps_t0 = now;
        s_fps_n++;
        if (now - s_fps_t0 >= 1000u) {
            serial_printf("[quake] %u FPS\n", (unsigned)((s_fps_n * 1000u) / (now - s_fps_t0)));
            s_fps_n = 0;
            s_fps_t0 = now;
        }
    }
}

void D_BeginDirectRect(int x, int y, byte *pbitmap, int width, int height) {
    (void)x; (void)y; (void)pbitmap; (void)width; (void)height;
}

void D_EndDirectRect(int x, int y, int width, int height) {
    (void)x; (void)y; (void)width; (void)height;
}
