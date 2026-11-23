#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <wm.h>
#include <vga.h>
#include <vga_gfx.h>
#include <driver/input/keyboard.h>
#include <system/state.h>
#include <system/timer.h>
#include <driver/audio_core.h>
#include <ipo/audio.h>
#include <driver/sound.h>
#include <syscall.h>
#include "common/ipo_hd_present.h"

extern int serial_printf(const char *fmt, ...);

typedef struct PokeredInstance PokeredInstance;
extern PokeredInstance *pokered_ipo_create(void);
extern void pokered_ipo_destroy(PokeredInstance *ptr);
extern void pokered_ipo_tick(PokeredInstance *ptr, uint8_t input_bits, uint8_t *out_rgba);
extern void pokered_ipo_render_audio(PokeredInstance *ptr, float *out_f32, size_t len);

/* GbButton bitmask: A=0, B=1, Select=2, Start=3, Right=4, Left=5, Up=6, Down=7 */
#define GB_BTN_A      (1u << 0)
#define GB_BTN_B      (1u << 1)
#define GB_BTN_SELECT (1u << 2)
#define GB_BTN_START  (1u << 3)
#define GB_BTN_RIGHT  (1u << 4)
#define GB_BTN_LEFT   (1u << 5)
#define GB_BTN_UP     (1u << 6)
#define GB_BTN_DOWN   (1u << 7)

#define GB_W 160
#define GB_H 144
#define CANVAS_W 214  /* ~4:3 pillarboxed aspect ratio */
#define CANVAS_H 144

static uint8_t s_wm_palette[248][3];
static uint8_t s_rgb555_to_wm[32768];
static bool    s_pal_ready = false;

static void init_wm_palette_lut(void) {
    if (s_pal_ready) return;
    static const uint8_t cga[16][3] = {
        {0, 0, 0},       {0, 0, 170},     {0, 170, 0},     {0, 170, 170},
        {170, 0, 0},     {170, 0, 170},   {170, 85, 0},    {170, 170, 170},
        {85, 85, 85},    {85, 85, 255},   {85, 255, 85},   {85, 255, 255},
        {255, 85, 85},   {255, 85, 255},  {255, 255, 85},  {255, 255, 255}
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
    for (int r5 = 0; r5 < 32; r5++) {
        int r = (r5 << 3) | (r5 >> 2);
        for (int g5 = 0; g5 < 32; g5++) {
            int g = (g5 << 3) | (g5 >> 2);
            for (int b5 = 0; b5 < 32; b5++) {
                int b = (b5 << 3) | (b5 >> 2);
                int best = 0;
                uint32_t best_d = 0xFFFFFFFFu;
                for (int i = 0; i < 248; i++) {
                    int dr = r - (int)s_wm_palette[i][0];
                    int dg = g - (int)s_wm_palette[i][1];
                    int db = b - (int)s_wm_palette[i][2];
                    uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
                    if (d < best_d) {
                        best_d = d;
                        best = i;
                        if (d == 0) break;
                    }
                }
                s_rgb555_to_wm[(r5 << 10) | (g5 << 5) | b5] = (uint8_t)best;
            }
        }
    }
    s_pal_ready = true;
}

static inline uint8_t rgb_to_wm(uint8_t r, uint8_t g, uint8_t b) {
    return s_rgb555_to_wm[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
}

static void apply_wm_dac_palette(void) {
    init_wm_palette_lut();
    for (int i = 0; i < 248; i++) {
        vga_gfx_set_palette((uint8_t)i,
                            (uint8_t)(s_wm_palette[i][0] >> 2),
                            (uint8_t)(s_wm_palette[i][1] >> 2),
                            (uint8_t)(s_wm_palette[i][2] >> 2));
    }
}

static uint8_t s_input_bits = 0;
static bool    s_should_quit = false;

static void handle_scancode(uint8_t sc, bool e0, bool release) {
    uint8_t bit = 0;
    if (e0) {
        switch (sc) {
            case 0x48: bit = GB_BTN_UP; break;
            case 0x50: bit = GB_BTN_DOWN; break;
            case 0x4B: bit = GB_BTN_LEFT; break;
            case 0x4D: bit = GB_BTN_RIGHT; break;
        }
    } else {
        switch (sc) {
            case 0x01: if (!release) s_should_quit = true; return; /* Esc */
            case 0x2C: /* Z */
            case 0x39: /* Space */
            case 0x24: /* J */
                bit = GB_BTN_A; break;
            case 0x2D: /* X */
            case 0x0E: /* Backspace */
            case 0x25: /* K */
                bit = GB_BTN_B; break;
            case 0x1C: /* Enter */
                bit = GB_BTN_START; break;
            case 0x0F: /* Tab */
            case 0x36: /* Right Shift */
                bit = GB_BTN_SELECT; break;
            case 0x11: bit = GB_BTN_UP; break;    /* W */
            case 0x1F: bit = GB_BTN_DOWN; break;  /* S */
            case 0x1E: bit = GB_BTN_LEFT; break;  /* A */
            case 0x20: bit = GB_BTN_RIGHT; break; /* D */
        }
    }
    if (bit != 0) {
        if (release) s_input_bits &= (uint8_t)~bit;
        else         s_input_bits |= bit;
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    init_wm_palette_lut();

    serial_printf("[pokered] initializing open-pokered engine...\n");
    PokeredInstance *inst = pokered_ipo_create();
    if (!inst) {
        serial_printf("[pokered] FATAL: failed to initialize PokeredInstance\n");
        return 1;
    }
    serial_printf("[pokered] open-pokered engine initialized!\n");

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    wm_window_t *wm_win = NULL;
    bool direct_gfx = false;
    {
        wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
        opt.title = "Pokemon Red (open-pokered)";
        opt.bg_color = 0;
        opt.fullscreen = true;
        opt.w = (uint16_t)sw;
        opt.h = (uint16_t)sh;
        wm_win = wm_create_window(&opt);
    }
    if (wm_win) {
        wm_set_fullscreen(wm_win, true);
        wm_set_cursor_visible(wm_win, false);
        wm_compositor_tick();
    } else {
        vga_set_mode_13h();
        apply_wm_dac_palette();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        direct_gfx = true;
    }

    uint8_t *rgba_fb = (uint8_t *)malloc(GB_W * GB_H * 4u);
    uint8_t *gb8 = (uint8_t *)calloc(GB_W * GB_H, 1u);
    ipo_hd_view_t hd;
    memset(&hd, 0, sizeof(hd));
    ipo_hd_setup(&hd, wm_win, GB_W, GB_H, true);
    float pcm_f32[735 * 2];

    bool audio_ok = audio_init();
    int16_t audio_ring[8192];
    size_t ring_pos = 0;
    audio_stream_t *astream = NULL;
    if (audio_ok) {
        memset(audio_ring, 0, sizeof(audio_ring));
        astream = audio_stream_create_ring(AUDIO_FORMAT_S16, 2, 44100u, audio_ring, 4096u, 0u);
        if (astream) audio_stream_play(astream);
    }

    uint32_t fps_t0 = timer_millis();
    uint32_t fps_frames = 0;
    uint32_t next_frame_ms = fps_t0;

    while (!s_should_quit) {
        if (direct_gfx && system_is_interrupted()) {
            system_clear_interrupt();
            break;
        }
        uint32_t now = timer_millis();
        if (now < next_frame_ms) {
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
            continue;
        }
        next_frame_ms += 16u; /* 60 FPS */
        if ((int32_t)(now - next_frame_ms) > 64) {
            next_frame_ms = now + 16u;
        }

        if (wm_win) {
            if (!wm_is_window_valid(wm_win) || !wm_session_active()) break;
            wm_event_t ev;
            while (wm_poll_event(wm_win, &ev)) {
                if (ev.type == WM_EVENT_CLOSE) { s_should_quit = true; break; }
                if (ev.type == WM_EVENT_KEY_EXT) {
                    bool rel = (ev.data & 0x80000000u) != 0u;
                    uint32_t kc = ev.data & 0x7FFFFFFFu;
                    handle_scancode((uint8_t)(kc & 0x7Fu), (kc & 0xFF00u) == 0xE000u, rel);
                } else if (ev.type == WM_EVENT_KEY_DOWN || ev.type == WM_EVENT_KEY_UP) {
                    static bool s_wm_e0 = false;
                    uint8_t raw = (uint8_t)(ev.data & 0xFFu);
                    if (raw == 0xE0u) { s_wm_e0 = true; continue; }
                    bool rel = (ev.type == WM_EVENT_KEY_UP) || ((raw & 0x80u) != 0u);
                    handle_scancode(raw & 0x7Fu, s_wm_e0, rel);
                    s_wm_e0 = false;
                }
            }
        } else {
            static bool s_dir_e0 = false;
            uint8_t raw;
            keyboard_poll();
            while ((raw = keyboard_get_scancode()) != 0u) {
                if (raw == 0xE0u) { s_dir_e0 = true; continue; }
                bool rel = (raw & 0x80u) != 0u;
                handle_scancode(raw & 0x7Fu, s_dir_e0, rel);
                s_dir_e0 = false;
            }
        }
        if (s_should_quit) break;

        pokered_ipo_tick(inst, s_input_bits, rgba_fb);

        for (int y = 0; y < GB_H; y++) {
            const uint8_t *srow = rgba_fb + (size_t)y * GB_W * 4u;
            uint8_t *drow = gb8 + (size_t)y * GB_W;
            for (int x = 0; x < GB_W; x++) {
                drow[x] = rgb_to_wm(srow[x * 4 + 0], srow[x * 4 + 1], srow[x * 4 + 2]);
            }
        }

        ipo_hd_present(&hd, wm_win, direct_gfx, gb8, GB_W, GB_H, GB_W);

        if (astream) {
            ipo_audio_clock_t clk;
            memset(&clk, 0, sizeof(clk));
            clk.struct_size = (uint32_t)sizeof(clk);
            uint64_t frames_written = (uint64_t)(ring_pos / 2u);
            bool has_room = true;
            if (ipo_audio_get_stream_clock(astream, &clk) == IPO_AUDIO_OK) {
                uint64_t ahead = (frames_written > clk.played_frames) ? (frames_written - clk.played_frames) : 0ULL;
                if (ahead + 735u >= 4096u - 128u) {
                    has_room = false;
                }
            }
            if (has_room) {
                pokered_ipo_render_audio(inst, pcm_f32, 735 * 2);
                for (int i = 0; i < 735 * 2; i++) {
                    float v = pcm_f32[i];
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    audio_ring[ring_pos % 8192u] = (int16_t)(v * 24000.0f);
                    ring_pos++;
                }
                audio_stream_update_buffered(astream, ring_pos / 2u, false);
            }
            audio_tick();
        }

        fps_frames++;
        uint32_t elapsed = now - fps_t0;
        if (elapsed >= 1000u) {
            serial_printf("[pokered] %u FPS\n", (fps_frames * 1000u) / elapsed);
            fps_frames = 0;
            fps_t0 = now;
        }
    }

    if (astream) {
        audio_stream_stop(astream);
        audio_stream_destroy(astream);
    }
    ipo_hd_free(&hd);
    free(rgba_fb);
    free(gb8);
    pokered_ipo_destroy(inst);

    if (wm_win && wm_is_window_valid(wm_win)) {
        wm_set_cursor_visible(wm_win, true);
        wm_destroy_window(wm_win);
    } else if (direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
    }
    return 0;
}
