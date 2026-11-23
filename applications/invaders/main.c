#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/audio_core.h>
#include <ipo/audio.h>
#include <driver/input/keyboard.h>
#include <driver/sound.h>
#include <syscall.h>
#include <system/state.h>
#include <system/timer.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>

#include "common/ipo_hd_present.h"
#include "emu-8080.h"
#include "invaders_rom.h"

#define CRT_W              224
#define CRT_H              256
#define CYCLES_PER_HALF    16667
#define AUDIO_FREQ         22050u
#define AUDIO_RING_SAMPLES 8192u

enum {
    SND_SHOOT = 0,
    SND_EXPLOSION,
    SND_INVADER_KILLED,
    SND_FAST_1,
    SND_FAST_2,
    SND_FAST_3,
    SND_FAST_4,
    SND_UFO_HIGH,
    SND_UFO_LOW,
    SND_COUNT
};

static const char *s_wav_names[SND_COUNT] = {
    "shoot.wav",
    "explosion.wav",
    "invaderkilled.wav",
    "fastinvader1.wav",
    "fastinvader2.wav",
    "fastinvader3.wav",
    "fastinvader4.wav",
    "ufo_highpitch.wav",
    "ufo_lowpitch.wav"
};

typedef struct {
    int16_t *samples;
    uint32_t count;
} sfx_clip_t;

typedef struct {
    const sfx_clip_t *clip;
    uint32_t pos;
    bool loop;
    bool active;
} sfx_voice_t;

typedef struct {
    uint8_t memory[0x4000];
    uint16_t shift_reg;
    uint8_t shift_amt;

    bool key_coin;
    bool key_p1_start;
    bool key_p2_start;
    bool key_left;
    bool key_right;
    bool key_fire;
    bool key_tilt;
    bool quit_req;
    bool ctrl_down;
    bool e0_prefix;

    uint8_t prev_out3;
    uint8_t prev_out5;

    uint8_t idx_black;
    uint8_t idx_white;
    uint8_t idx_red;
    uint8_t idx_green;
    uint8_t CRT[CRT_W * CRT_H];

    wm_window_t *wm_win;
    bool direct_gfx;
    ipo_hd_view_t hd;

    audio_stream_t *astream;
    int16_t audio_ring[AUDIO_RING_SAMPLES];
    uint32_t audio_write_samples;
    uint32_t audio_start_ms;
    uint64_t audio_total_written;
    sfx_clip_t clips[SND_COUNT];
    sfx_voice_t *voices;
    int voices_cap;
} inv_state_t;

static inv_state_t *g_inv = NULL;
static uint8_t s_wm_palette[248][3];

static void init_wm_palette_lut(void) {
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
}

static uint8_t rgb_to_wm_fast(uint8_t r, uint8_t g, uint8_t b) {
    int ri = ((int)r * 5 + 127) / 255;
    int gi = ((int)g * 5 + 127) / 255;
    int bi = ((int)b * 5 + 127) / 255;
    return (uint8_t)(32 + (ri * 6 + gi) * 6 + bi);
}

static void apply_wm_dac_palette(void) {
    for (int i = 0; i < 248; i++) {
        vga_gfx_set_palette((uint8_t)i,
                            (uint8_t)(s_wm_palette[i][0] >> 2),
                            (uint8_t)(s_wm_palette[i][1] >> 2),
                            (uint8_t)(s_wm_palette[i][2] >> 2));
    }
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool load_wav_pcm(const char *name, sfx_clip_t *out) {
    if (!name) return false;
    size_t nlen = strlen(name);
    if (nlen > ((size_t)-1) - 20u) return false;
    char *path = (char *)malloc(nlen + 20u);
    if (!path) return false;
    snprintf(path, nlen + 20u, "/data/invaders/wav/%s", name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(path, nlen + 20u, "data/invaders/wav/%s", name);
        f = fopen(path, "rb");
    }
    free(path);
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 44 || sz > 2 * 1024 * 1024) { fclose(f); return false; }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return false; }
    fclose(f);

    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf);
        return false;
    }

    uint16_t chans = 1, bps = 16;
    const uint8_t *pcm_ptr = NULL;
    uint32_t pcm_sz = 0;
    size_t pos = 12;
    while (pos + 8 <= (size_t)sz) {
        uint32_t csz = rd_u32(buf + pos + 4);
        if (memcmp(buf + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= (size_t)sz) {
            chans = rd_u16(buf + pos + 10);
            bps   = rd_u16(buf + pos + 22);
        } else if (memcmp(buf + pos, "data", 4) == 0 && pos + 8 + csz <= (size_t)sz) {
            pcm_ptr = buf + pos + 8;
            pcm_sz  = csz;
        }
        pos += 8 + ((csz + 1u) & ~1u);
    }

    if (!pcm_ptr || pcm_sz == 0 || bps != 16 || chans == 0) {
        free(buf);
        return false;
    }

    uint32_t frames = pcm_sz / (2u * chans);
    int16_t *mono = (int16_t *)malloc((size_t)frames * sizeof(int16_t));
    if (!mono) { free(buf); return false; }
    const int16_t *in16 = (const int16_t *)pcm_ptr;
    for (uint32_t i = 0; i < frames; i++) {
        mono[i] = (chans == 1) ? in16[i]
                               : (int16_t)(((int32_t)in16[i * chans] + (int32_t)in16[i * chans + 1]) >> 1);
    }
    free(buf);
    out->samples = mono;
    out->count = frames;
    return true;
}

static void play_sfx(inv_state_t *st, int id, bool loop) {
    if ((unsigned)id >= SND_COUNT || !st->clips[id].samples || st->clips[id].count == 0) return;
    if (loop) {
        for (int v = 0; v < st->voices_cap; v++) {
            if (st->voices[v].active && st->voices[v].clip == &st->clips[id]) return;
        }
    }
    int slot = -1;
    for (int v = 0; v < st->voices_cap; v++) {
        if (!st->voices[v].active) { slot = v; break; }
    }
    if (slot < 0) {
        int new_cap = st->voices_cap ? (st->voices_cap * 2) : 8;
        if (new_cap < st->voices_cap || (size_t)new_cap > SIZE_MAX / sizeof(sfx_voice_t)) return;
        sfx_voice_t *nv = (sfx_voice_t *)realloc(st->voices, (size_t)new_cap * sizeof(sfx_voice_t));
        if (!nv) return;
        memset(nv + st->voices_cap, 0, (size_t)(new_cap - st->voices_cap) * sizeof(sfx_voice_t));
        slot = st->voices_cap;
        st->voices = nv;
        st->voices_cap = new_cap;
    }
    st->voices[slot].clip = &st->clips[id];
    st->voices[slot].pos = 0;
    st->voices[slot].loop = loop;
    st->voices[slot].active = true;
}

static void stop_sfx(inv_state_t *st, int id) {
    if ((unsigned)id >= SND_COUNT) return;
    for (int v = 0; v < st->voices_cap; v++) {
        if (st->voices[v].active && st->voices[v].clip == &st->clips[id]) {
            st->voices[v].active = false;
        }
    }
}

static void audio_pump(inv_state_t *st) {
    if (!st->astream) return;
    ipo_audio_clock_t clk;
    memset(&clk, 0, sizeof(clk));
    clk.struct_size = (uint32_t)sizeof(clk);
    uint64_t played = 0ULL;
    if (ipo_audio_get_stream_clock(st->astream, &clk) == IPO_AUDIO_OK) {
        played = clk.played_frames;
    } else {
        uint32_t now = timer_millis();
        played = ((uint64_t)(now - st->audio_start_ms) * AUDIO_FREQ) / 1000ULL;
    }
    uint64_t target = played + (AUDIO_FREQ / 15u);
    if (target <= st->audio_total_written) {
        audio_tick();
        return;
    }
    uint64_t todo = target - st->audio_total_written;
    uint64_t ahead = (st->audio_total_written > played) ? (st->audio_total_written - played) : 0ULL;
    uint64_t max_safe = (AUDIO_RING_SAMPLES > ahead + 128u) ? (AUDIO_RING_SAMPLES - ahead - 128u) : 0ULL;
    if (todo > max_safe) todo = max_safe;
    if (todo > 2048u) todo = 2048u;
    if (todo == 0u) { audio_tick(); return; }
    for (uint64_t i = 0; i < todo; i++) {
        int32_t acc = 0;
        for (int v = 0; v < st->voices_cap; v++) {
            sfx_voice_t *vc = &st->voices[v];
            if (!vc->active || !vc->clip || vc->clip->count == 0) continue;
            if (vc->pos >= vc->clip->count) {
                if (vc->loop) vc->pos = 0;
                else { vc->active = false; continue; }
            }
            acc += vc->clip->samples[vc->pos++];
        }
        if (acc > 32767) acc = 32767;
        if (acc < -32768) acc = -32768;
        st->audio_ring[st->audio_write_samples % AUDIO_RING_SAMPLES] = (int16_t)acc;
        st->audio_write_samples++;
        st->audio_total_written++;
    }
    audio_stream_update_buffered(st->astream, st->audio_write_samples, false);
    audio_tick();
}

static int inv_port_read(void *g, int p) {
    inv_state_t *st = (inv_state_t *)g;
    switch (p & 7) {
        case 0: {
            uint8_t v = 0x0E;
            if (st->key_fire)  v |= 0x10u;
            if (st->key_left)  v |= 0x20u;
            if (st->key_right) v |= 0x40u;
            return v;
        }
        case 1: {
            uint8_t v = 0x08;
            if (st->key_coin)     v |= 0x01u;
            if (st->key_p2_start) v |= 0x02u;
            if (st->key_p1_start) v |= 0x04u;
            if (st->key_fire)     v |= 0x10u;
            if (st->key_left)     v |= 0x20u;
            if (st->key_right)    v |= 0x40u;
            return v;
        }
        case 2: {
            uint8_t v = 0x00; /* DIP: 3 ships, bonus at 1500 */
            if (st->key_tilt)  v |= 0x04u;
            if (st->key_fire)  v |= 0x10u;
            if (st->key_left)  v |= 0x20u;
            if (st->key_right) v |= 0x40u;
            return v;
        }
        case 3:
            return (st->shift_reg >> (8 - st->shift_amt)) & 0xFF;
        default:
            return 0;
    }
}

static void inv_port_write(void *g, int p, int val) {
    inv_state_t *st = (inv_state_t *)g;
    uint8_t v = (uint8_t)(val & 0xFF);
    switch (p & 7) {
        case 2:
            st->shift_amt = v & 7u;
            break;
        case 4:
            st->shift_reg = (uint16_t)((st->shift_reg >> 8) | ((uint16_t)v << 8));
            break;
        case 3: {
            uint8_t rise = v & (uint8_t)~st->prev_out3;
            if (v & 0x01u) play_sfx(st, SND_UFO_LOW, true);
            else           stop_sfx(st, SND_UFO_LOW);
            if (rise & 0x02u) play_sfx(st, SND_SHOOT, false);
            if (rise & 0x04u) play_sfx(st, SND_EXPLOSION, false);
            if (rise & 0x08u) play_sfx(st, SND_INVADER_KILLED, false);
            st->prev_out3 = v;
            break;
        }
        case 5: {
            uint8_t rise = v & (uint8_t)~st->prev_out5;
            if (rise & 0x01u) play_sfx(st, SND_FAST_1, false);
            if (rise & 0x02u) play_sfx(st, SND_FAST_2, false);
            if (rise & 0x04u) play_sfx(st, SND_FAST_3, false);
            if (rise & 0x08u) play_sfx(st, SND_FAST_4, false);
            if (rise & 0x10u) play_sfx(st, SND_UFO_HIGH, false);
            st->prev_out5 = v;
            break;
        }
        default:
            break;
    }
}

static void handle_scancode(inv_state_t *st, uint8_t sc, bool e0, bool down) {
    if (sc == 0x1D) { st->ctrl_down = down; return; }
    if (down && st->ctrl_down && (sc == 0x2E || sc == 0x10)) { st->quit_req = true; return; }
    if (!e0 && sc == 0x01 && down) { st->quit_req = true; return; } /* ESC exits */

    if (e0) {
        if (sc == 0x4B) st->key_left = down;
        else if (sc == 0x4D) st->key_right = down;
        else if (sc == 0x48) st->key_fire = down;
        return;
    }
    switch (sc) {
        case 0x2E: /* C */
        case 0x17: /* I */
        case 0x06: /* 5 */
        case 0x1C: /* Enter */
            st->key_coin = down;
            break;
        case 0x02: /* 1 */
            st->key_p1_start = down;
            break;
        case 0x03: /* 2 */
            st->key_p2_start = down;
            break;
        case 0x1E: /* A */
        case 0x2C: /* Z */
            st->key_left = down;
            break;
        case 0x20: /* D */
            st->key_right = down;
            break;
        case 0x39: /* Space */
        case 0x2D: /* X */
        case 0x11: /* W */
            st->key_fire = down;
            break;
        case 0x14: /* T */
            st->key_tilt = down;
            break;
        default:
            break;
    }
}

static void poll_inputs(inv_state_t *st) {
    if (st->wm_win) {
        if (!wm_is_window_valid(st->wm_win) || !wm_session_active()) {
            st->quit_req = true;
            return;
        }
        wm_event_t we;
        while (wm_poll_event(st->wm_win, &we)) {
            if (we.type == WM_EVENT_CLOSE) {
                st->quit_req = true;
            } else if (we.type == WM_EVENT_KEY_EXT) {
                bool rel = (we.data & 0x80000000u) != 0u;
                uint32_t kc = we.data & 0x7FFFFFFFu;
                bool e0 = ((kc & 0xFF00u) == 0xE000u);
                handle_scancode(st, (uint8_t)(kc & 0x7Fu), e0, !rel);
            } else if (we.type == WM_EVENT_KEY_DOWN || we.type == WM_EVENT_KEY_UP) {
                uint8_t raw = (uint8_t)(we.data & 0xFFu);
                if (raw == 0xE0u) { st->e0_prefix = true; continue; }
                bool rel = (we.type == WM_EVENT_KEY_UP) || ((raw & 0x80u) != 0u);
                handle_scancode(st, raw & 0x7Fu, st->e0_prefix, !rel);
                st->e0_prefix = false;
            }
        }
    } else if (st->direct_gfx) {
        if (system_is_interrupted()) {
            system_clear_interrupt();
            st->quit_req = true;
            return;
        }
        keyboard_poll();
        uint8_t raw;
        while ((raw = keyboard_get_scancode()) != 0u) {
            if (raw == 0xE0u) { st->e0_prefix = true; continue; }
            bool rel = (raw & 0x80u) != 0u;
            handle_scancode(st, raw & 0x7Fu, st->e0_prefix, !rel);
            st->e0_prefix = false;
        }
    }
}

static void render_crt(inv_state_t *st) {
    /* Rotate 256x224 1-bit VRAM (0x2400..0x3FFF) -90 deg into 224x256 with arcade cellophane colors */
    const uint8_t *vram = &st->memory[0x2400];
    for (int col = 0; col < CRT_W; col++) {
        const uint8_t *col_bytes = vram + col * 32;
        for (int b = 0; b < 32; b++) {
            uint8_t byte = col_bytes[b];
            int y_base = 255 - (b * 8);
            for (int bit = 0; bit < 8; bit++) {
                int y = y_base - bit;
                uint8_t c = st->idx_black;
                if ((byte >> bit) & 1u) {
                    if (y >= 32 && y < 64) c = st->idx_red;
                    else if ((y >= 184 && y < 240) || (y >= 240 && col >= 16 && col < 136)) c = st->idx_green;
                    else c = st->idx_white;
                }
                st->CRT[y * CRT_W + col] = c;
            }
        }
    }

    ipo_hd_present(&st->hd, st->wm_win, st->direct_gfx, st->CRT, CRT_W, CRT_H, CRT_W);
}

int main(void) {
    init_wm_palette_lut();
    g_inv = (inv_state_t *)calloc(1, sizeof(inv_state_t));
    if (!g_inv) return 1;

    g_inv->idx_black = rgb_to_wm_fast(0, 0, 0);
    g_inv->idx_white = rgb_to_wm_fast(255, 255, 255);
    g_inv->idx_red   = rgb_to_wm_fast(255, 51, 51);
    g_inv->idx_green = rgb_to_wm_fast(51, 255, 102);

    memcpy(g_inv->memory, invaders_rom, invaders_rom_len);

    for (int i = 0; i < SND_COUNT; i++) {
        load_wav_pcm(s_wav_names[i], &g_inv->clips[i]);
    }
    if (audio_init()) {
        g_inv->audio_start_ms = timer_millis();
        g_inv->astream = audio_stream_create_ring(AUDIO_FORMAT_S16, 1u, AUDIO_FREQ,
                                                  g_inv->audio_ring, AUDIO_RING_SAMPLES, 0u);
        if (g_inv->astream) audio_stream_play(g_inv->astream);
    }

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Space Invaders (1978)";
    opt.bg_color = 0;
    opt.w = (uint16_t)sw;
    opt.h = (uint16_t)sh;
    g_inv->wm_win = wm_create_window(&opt);
    if (g_inv->wm_win) {
        wm_set_fullscreen(g_inv->wm_win, true);
        wm_set_cursor_visible(g_inv->wm_win, false);
        wm_compositor_tick();
        if (g_inv->wm_win->w > 0 && g_inv->wm_win->h > 0) {
            sw = (int)g_inv->wm_win->w;
            sh = (int)g_inv->wm_win->h;
        }
    } else {
        vga_set_mode_13h();
        apply_wm_dac_palette();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        g_inv->direct_gfx = true;
    }

    ipo_hd_setup(&g_inv->hd, g_inv->wm_win, CRT_W, CRT_H, true);

    struct Context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.memory = g_inv->memory;
    ctx.port_read = inv_port_read;
    ctx.port_write = inv_port_write;
    ctx.address_mask = 0x3FFF;
    ctx.rom_size = 0x2000;
    ctx.gData = g_inv;

    serial_printf("[invaders] started (rom=%u bytes scr=%dx%d pres=(%d,%d %dx%d) k=%d)\n",
                  invaders_rom_len, g_inv->hd.scr_w, g_inv->hd.scr_h,
                  g_inv->hd.pres_x, g_inv->hd.pres_y, g_inv->hd.pres_w, g_inv->hd.pres_h, g_inv->hd.scale_k);

    uint32_t fps_t0 = timer_millis();
    uint32_t fps_frames = 0;
    uint32_t next_frame_ms = fps_t0;

    while (!g_inv->quit_req) {
        poll_inputs(g_inv);

        int cycles = 0;
        while (cycles < CYCLES_PER_HALF) {
            cycles += emu_8080_execute(&ctx);
        }
        if (ctx.interrupt) emu_8080_rst(&ctx, 1);

        cycles = 0;
        while (cycles < CYCLES_PER_HALF) {
            cycles += emu_8080_execute(&ctx);
        }
        if (ctx.interrupt) emu_8080_rst(&ctx, 2);

        render_crt(g_inv);
        audio_pump(g_inv);

        fps_frames++;
        uint32_t now = timer_millis();
        if (now - fps_t0 >= 1000u) {
            serial_printf("[invaders] %u FPS\n", (fps_frames * 1000u) / (now - fps_t0));
            fps_frames = 0;
            fps_t0 = now;
        }

        next_frame_ms += (fps_frames % 3u == 0u) ? 16u : 17u;
        int32_t rem = (int32_t)(next_frame_ms - timer_millis());
        if (rem < -100) {
            next_frame_ms = timer_millis();
        } else {
            while ((int32_t)(next_frame_ms - timer_millis()) > 0) {
                audio_pump(g_inv);
                ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
            }
        }
    }

    if (g_inv->astream) {
        audio_stream_stop(g_inv->astream);
        audio_stream_destroy(g_inv->astream);
    }
    if (g_inv->wm_win && wm_is_window_valid(g_inv->wm_win)) {
        wm_set_cursor_visible(g_inv->wm_win, true);
        wm_destroy_window(g_inv->wm_win);
    } else if (g_inv->direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
    }
    ipo_hd_free(&g_inv->hd);
    free(g_inv->voices);
    g_inv->voices = NULL;
    g_inv->voices_cap = 0;
    for (int i = 0; i < SND_COUNT; i++) free(g_inv->clips[i].samples);
    free(g_inv);
    return 0;
}
