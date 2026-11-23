#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/input/keyboard.h>
#include <syscall.h>
#include <system/state.h>
#include <system/timer.h>
#include <vga.h>
#include <vga_gfx.h>
#include <wm.h>

#include "common/ipo_hd_present.h"
#include "spacewar_rom.h"

#define MEM_SIZE        4096
#define MASK18          0777777u
#define SIGN18          0400000u
#define MASK12          07777u
#define CRT_DIM         512
#define CYCLES_PER_TICK 20000

typedef struct {
    uint32_t M[MEM_SIZE];
    uint32_t AC;
    uint32_t IO;
    uint32_t PC;
    uint32_t TW;
    uint8_t OV;
    uint8_t flags;
    uint8_t sense;

    /* Controls for Ship 1 (bits 17..14) & Ship 2 (bits 3..0) */
    bool s1_ccw, s1_cw, s1_thrust, s1_fire, s1_hyper;
    bool s2_ccw, s2_cw, s2_thrust, s2_fire, s2_hyper;
    bool quit_req;
    bool halted;
    bool ctrl_down;
    bool e0_prefix;

    uint8_t phos[CRT_DIM * CRT_DIM];
    uint8_t CRT[CRT_DIM * CRT_DIM];
    uint8_t pal_phos[256];

    wm_window_t *wm_win;
    bool direct_gfx;
    ipo_hd_view_t hd;
} pdp1_t;

static pdp1_t *g_pdp = NULL;
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

static inline uint32_t add18(uint32_t a, uint32_t b, uint8_t *ov) {
    uint32_t s = a + b;
    if (s & 01000000u) s = (s + 1u) & MASK18;
    /* DEC PDP-1 hardware adder eliminates -0 except when (-0) + (-0) */
    if (s == MASK18 && !(a == MASK18 && b == MASK18)) s = 0u;
    if (ov) {
        if (!((a ^ b) & SIGN18) && ((a ^ s) & SIGN18)) *ov = 1;
    }
    return s;
}

static inline uint32_t sub18(uint32_t a, uint32_t b, uint8_t *ov) {
    return add18(a, (~b) & MASK18, ov);
}

static inline int popcount9(uint32_t v) {
    int c = 0;
    for (int i = 0; i < 9; i++) {
        if (v & (1u << i)) c++;
    }
    return c;
}

static void plot_crt_dot(pdp1_t *p, uint32_t ac, uint32_t io) {
    int x10 = (((int)(ac >> 8)) ^ 512) & 1023;
    int y10 = 1023 - ((((int)(io >> 8)) ^ 512) & 1023);
    int px = x10 >> 1;
    int py = y10 >> 1;

    /* Type 30 CRT P7 phosphor beam excitation on 512x512 CRT */
    p->phos[py * CRT_DIM + px] = 255u;
    if (px > 0 && p->phos[py * CRT_DIM + px - 1] < 130u) p->phos[py * CRT_DIM + px - 1] = 130u;
    if (px + 1 < CRT_DIM && p->phos[py * CRT_DIM + px + 1] < 130u) p->phos[py * CRT_DIM + px + 1] = 130u;
    if (py > 0 && p->phos[(py - 1) * CRT_DIM + px] < 130u) p->phos[(py - 1) * CRT_DIM + px] = 130u;
    if (py + 1 < CRT_DIM && p->phos[(py + 1) * CRT_DIM + px] < 130u) p->phos[(py + 1) * CRT_DIM + px] = 130u;
}

static uint32_t read_control_word(const pdp1_t *p) {
    uint32_t cw = 0;
    if (p->s1_ccw || p->s1_hyper)   cw |= 0400000u;
    if (p->s1_cw  || p->s1_hyper)   cw |= 0200000u;
    if (p->s1_thrust)               cw |= 0100000u;
    if (p->s1_fire)                 cw |= 0040000u;

    if (p->s2_ccw || p->s2_hyper)   cw |= 0000010u;
    if (p->s2_cw  || p->s2_hyper)   cw |= 0000004u;
    if (p->s2_thrust)               cw |= 0000002u;
    if (p->s2_fire)                 cw |= 0000001u;
    return cw;
}

static void pdp1_exec_word(pdp1_t *p, uint32_t ins) {
    uint32_t op = (ins >> 13) & 037u;
    bool ib = ((ins >> 12) & 1u) != 0u;
    uint32_t y = ins & MASK12;

    if (op >= 001u && op <= 031u && op != 007u) {
        if (ib) y = p->M[y] & MASK12;
    }

    switch (op) {
        case 001: /* and */
            p->AC &= p->M[y];
            break;
        case 002: /* ior */
            p->AC |= p->M[y];
            break;
        case 003: /* xor */
            p->AC ^= p->M[y];
            break;
        case 004: /* xct */
            pdp1_exec_word(p, p->M[y]);
            break;
        case 007: /* cal / jda */
            if (!ib) y = 0100u;
            p->M[y] = p->AC;
            p->AC = ((uint32_t)(p->OV & 1u) << 17) | (p->PC & MASK12);
            p->PC = (y + 1u) & MASK12;
            break;
        case 010: /* lac */
            p->AC = p->M[y];
            break;
        case 011: /* lio */
            p->IO = p->M[y];
            break;
        case 012: /* dac */
            p->M[y] = p->AC;
            break;
        case 013: /* dap */
            p->M[y] = (p->M[y] & 0770000u) | (p->AC & MASK12);
            break;
        case 014: /* dip */
            p->M[y] = (p->M[y] & MASK12) | (p->AC & 0770000u);
            break;
        case 015: /* dio */
            p->M[y] = p->IO;
            break;
        case 016: /* dzm */
            p->M[y] = 0u;
            break;
        case 020: /* add */
            p->AC = add18(p->AC, p->M[y], &p->OV);
            break;
        case 021: /* sub */
            p->AC = sub18(p->AC, p->M[y], &p->OV);
            break;
        case 022: /* idx */
            p->AC = add18(p->M[y], 1u, NULL);
            p->M[y] = p->AC;
            break;
        case 023: /* isp */
            p->AC = add18(p->M[y], 1u, NULL);
            p->M[y] = p->AC;
            if (!(p->AC & SIGN18)) p->PC = (p->PC + 1u) & MASK12;
            break;
        case 024: /* sad */
            if (p->AC != p->M[y]) p->PC = (p->PC + 1u) & MASK12;
            break;
        case 025: /* sas */
            if (p->AC == p->M[y]) p->PC = (p->PC + 1u) & MASK12;
            break;
        case 026: /* mus (multiply step) */
            if (p->IO & 1u) p->AC = add18(p->AC, p->M[y], NULL);
            p->IO = ((p->AC & 1u) << 17) | (p->IO >> 1);
            p->AC = (p->AC >> 1);
            break;
        case 027: { /* dis (divide step) */
            uint32_t d = p->M[y];
            uint32_t cy = (p->AC >> 17) & 1u;
            uint32_t io_top = (p->IO >> 17) & 1u;
            if (cy) {
                uint32_t ac2 = ((p->AC << 1) | 1u) & MASK18;
                if (io_top) ac2 = add18(ac2, 1u, NULL);
                p->AC = add18(ac2, d, NULL);
            } else {
                uint32_t ac2 = ((p->AC << 1) | io_top) & MASK18;
                p->AC = sub18(ac2, d, NULL);
            }
            if (p->AC == MASK18) p->AC = 0u;
            p->IO = ((p->IO << 1) | (cy ^ 1u)) & MASK18;
            break;
        }
        case 030: /* jmp */
            p->PC = y;
            break;
        case 031: /* jsp */
            p->AC = ((uint32_t)(p->OV & 1u) << 17) | (p->PC & MASK12);
            p->PC = y;
            break;
        case 032: { /* skp */
            bool cond = false;
            if ((ins & 000100u) && (p->AC == 0u)) cond = true;             /* sza */
            if ((ins & 000200u) && !(p->AC & SIGN18)) cond = true;         /* spa */
            if ((ins & 000400u) && (p->AC & SIGN18)) cond = true;          /* sma */
            if (ins & 001000u) { if (p->OV == 0u) cond = true; p->OV = 0; }/* szo */
            if ((ins & 002000u) && !(p->IO & SIGN18)) cond = true;         /* spi */
            uint32_t sw_mask = (ins >> 3) & 07u;
            if (sw_mask) {
                uint8_t bit = (uint8_t)(1u << (6 - sw_mask));
                if ((p->sense & bit) == 0u) cond = true;                   /* szs */
            }
            uint32_t fl_mask = ins & 07u;
            if (fl_mask) {
                if (fl_mask == 7u) {
                    if ((p->flags & 077u) == 0u) cond = true;
                } else {
                    uint8_t bit = (uint8_t)(1u << (6 - fl_mask));
                    if ((p->flags & bit) == 0u) cond = true;               /* szf */
                }
            }
            if (ib) cond = !cond;
            if (cond) p->PC = (p->PC + 1u) & MASK12;
            break;
        }
        case 033: { /* shf */
            int cnt = popcount9(ins & 0777u);
            uint32_t kind = (ins >> 9) & 017u;
            bool right = (kind & 010u) != 0u;
            bool arith = (kind & 004u) != 0u;
            uint32_t mode = kind & 003u; /* 1=AC, 2=IO, 3=AC+IO */
            for (int i = 0; i < cnt; i++) {
                if (!right) {
                    if (mode == 1) {
                        uint32_t msb = (p->AC >> 17) & 1u;
                        uint32_t fill = arith ? (p->AC & 1u) : msb;
                        uint32_t sgn = p->AC & SIGN18;
                        p->AC = ((p->AC << 1) | fill) & MASK18;
                        if (arith) p->AC = (p->AC & ~SIGN18) | sgn;
                    } else if (mode == 2) {
                        uint32_t msb = (p->IO >> 17) & 1u;
                        uint32_t sgn = p->IO & SIGN18;
                        p->IO = ((p->IO << 1) | msb) & MASK18;
                        if (arith) p->IO = (p->IO & ~SIGN18) | sgn;
                    } else if (mode == 3) {
                        uint32_t ac_msb = (p->AC >> 17) & 1u;
                        uint32_t io_msb = (p->IO >> 17) & 1u;
                        uint32_t sgn = p->AC & SIGN18;
                        p->AC = ((p->AC << 1) | io_msb) & MASK18;
                        p->IO = ((p->IO << 1) | ac_msb) & MASK18;
                        if (arith) p->AC = (p->AC & ~SIGN18) | sgn;
                    }
                } else {
                    if (mode == 1) {
                        uint32_t fill = arith ? (p->AC & SIGN18) : ((p->AC & 1u) << 17);
                        p->AC = ((p->AC >> 1) | fill) & MASK18;
                    } else if (mode == 2) {
                        uint32_t fill = arith ? (p->IO & SIGN18) : ((p->IO & 1u) << 17);
                        p->IO = ((p->IO >> 1) | fill) & MASK18;
                    } else if (mode == 3) {
                        uint32_t ac_lsb = (p->AC & 1u) << 17;
                        uint32_t io_lsb = (p->IO & 1u) << 17;
                        uint32_t fill = arith ? (p->AC & SIGN18) : io_lsb;
                        p->AC = ((p->AC >> 1) | fill) & MASK18;
                        p->IO = ((p->IO >> 1) | ac_lsb) & MASK18;
                    }
                }
            }
            break;
        }
        case 034: /* law */
            p->AC = ib ? ((~y) & MASK18) : y;
            break;
        case 035: { /* iot */
            uint32_t dev = ins & 077u;
            if (dev == 007u) {
                plot_crt_dot(p, p->AC, p->IO);
            } else if (dev == 011u) {
                p->IO = read_control_word(p);
            }
            break;
        }
        case 037: { /* opr */
            if (ins & 004000u) p->IO = 0u;                           /* cli */
            if (ins & 000200u) p->AC = 0u;                           /* cla */
            if (ins & 002000u) p->AC |= read_control_word(p);        /* lat */
            if (ins & 000100u) p->AC |= (p->PC & MASK12);            /* lap */
            if (ins & 001000u) p->AC = (~p->AC) & MASK18;            /* cma */
            if (ins & 000400u) p->halted = true;                     /* hlt */
            uint32_t fn = ins & 000007u;
            if (fn) {
                bool stf = (ins & 000010u) != 0u;
                if (fn == 7u) {
                    p->flags = stf ? 077u : 0u;
                } else {
                    uint8_t bit = (uint8_t)(1u << (6 - fn));
                    if (stf) p->flags |= bit;
                    else     p->flags &= (uint8_t)~bit;
                }
            }
            break;
        }
        default:
            break;
    }
}

static void pdp1_step(pdp1_t *p) {
    uint32_t ins = p->M[p->PC & MASK12];
    p->PC = (p->PC + 1u) & MASK12;
    pdp1_exec_word(p, ins);
}

static void handle_scancode(pdp1_t *p, uint8_t sc, bool e0, bool down) {
    if (sc == 0x1D) { p->ctrl_down = down; return; }
    if (down && p->ctrl_down && (sc == 0x2E || sc == 0x10)) { p->quit_req = true; return; }
    if (!e0 && sc == 0x01 && down) { p->quit_req = true; return; } /* ESC exits */

    if (e0) {
        if (sc == 0x4B) p->s1_ccw = down;
        else if (sc == 0x4D) p->s1_cw = down;
        else if (sc == 0x48) p->s1_thrust = down;
        else if (sc == 0x50) p->s1_fire = down;
        return;
    }
    switch (sc) {
        /* Player 1 (Needle): A/D rotate, W thrust, Space/S fire, Q hyperspace */
        case 0x1E: p->s1_ccw = down; break;
        case 0x20: p->s1_cw = down; break;
        case 0x11: p->s1_thrust = down; break;
        case 0x1F:
        case 0x39: p->s1_fire = down; break;
        case 0x10: p->s1_hyper = down; break;
        /* Player 2 (Wedge): J/L rotate, I thrust, K/Enter fire, U hyperspace */
        case 0x24: p->s2_ccw = down; break;
        case 0x26: p->s2_cw = down; break;
        case 0x17: p->s2_thrust = down; break;
        case 0x25:
        case 0x1C: p->s2_fire = down; break;
        case 0x16: p->s2_hyper = down; break;
        /* Sense switches 1..6 via keys 1..6 */
        case 0x02: if (down) p->sense ^= 0x20u; break;
        case 0x03: if (down) p->sense ^= 0x10u; break;
        case 0x04: if (down) p->sense ^= 0x08u; break;
        case 0x05: if (down) p->sense ^= 0x04u; break;
        case 0x06: if (down) p->sense ^= 0x02u; break;
        case 0x07: if (down) p->sense ^= 0x01u; break;
        default: break;
    }
}

static void poll_inputs(pdp1_t *p) {
    if (p->wm_win) {
        if (!wm_is_window_valid(p->wm_win) || !wm_session_active()) {
            p->quit_req = true;
            return;
        }
        wm_event_t we;
        while (wm_poll_event(p->wm_win, &we)) {
            if (we.type == WM_EVENT_CLOSE) {
                p->quit_req = true;
            } else if (we.type == WM_EVENT_KEY_EXT) {
                bool rel = (we.data & 0x80000000u) != 0u;
                uint32_t kc = we.data & 0x7FFFFFFFu;
                bool e0 = ((kc & 0xFF00u) == 0xE000u);
                handle_scancode(p, (uint8_t)(kc & 0x7Fu), e0, !rel);
            } else if (we.type == WM_EVENT_KEY_DOWN || we.type == WM_EVENT_KEY_UP) {
                uint8_t raw = (uint8_t)(we.data & 0xFFu);
                if (raw == 0xE0u) { p->e0_prefix = true; continue; }
                bool rel = (we.type == WM_EVENT_KEY_UP) || ((raw & 0x80u) != 0u);
                handle_scancode(p, raw & 0x7Fu, p->e0_prefix, !rel);
                p->e0_prefix = false;
            }
        }
    } else if (p->direct_gfx) {
        if (system_is_interrupted()) {
            system_clear_interrupt();
            p->quit_req = true;
            return;
        }
        keyboard_poll();
        uint8_t raw;
        while ((raw = keyboard_get_scancode()) != 0u) {
            if (raw == 0xE0u) { p->e0_prefix = true; continue; }
            bool rel = (raw & 0x80u) != 0u;
            handle_scancode(p, raw & 0x7Fu, p->e0_prefix, !rel);
            p->e0_prefix = false;
        }
    }
}

static void render_crt(pdp1_t *p) {
    uint8_t bezel_col = p->pal_phos[36];
    for (int i = 0; i < CRT_DIM * CRT_DIM; i++) {
        uint8_t v = p->phos[i];
        p->CRT[i] = p->pal_phos[v];
        p->phos[i] = (uint8_t)(((uint32_t)v * 185u) >> 8);
    }
    for (int x = 0; x < CRT_DIM; x++) {
        p->CRT[x] = bezel_col;
        p->CRT[(CRT_DIM - 1) * CRT_DIM + x] = bezel_col;
    }
    for (int y = 0; y < CRT_DIM; y++) {
        p->CRT[y * CRT_DIM] = bezel_col;
        p->CRT[y * CRT_DIM + CRT_DIM - 1] = bezel_col;
    }
    ipo_hd_present(&p->hd, p->wm_win, p->direct_gfx, p->CRT, CRT_DIM, CRT_DIM, CRT_DIM);
}

int main(void) {
    init_wm_palette_lut();
    g_pdp = (pdp1_t *)calloc(1, sizeof(pdp1_t));
    if (!g_pdp) return 1;

    /* P7 Phosphor color ramp: dark CRT glass -> green afterglow -> bright cyan-white electron flash */
    for (int i = 0; i < 256; i++) {
        uint8_t r, g, b;
        if (i < 16) {
            r = 0; g = (uint8_t)(i / 2); b = (uint8_t)(i / 3);
        } else if (i < 160) {
            int t = i - 16;
            r = (uint8_t)(t / 4);
            g = (uint8_t)(40 + (t * 200) / 144);
            b = (uint8_t)(30 + (t * 150) / 144);
        } else {
            int t = i - 160;
            r = (uint8_t)(40 + (t * 215) / 95);
            g = (uint8_t)(240 + (t * 15) / 95);
            b = (uint8_t)(180 + (t * 75) / 95);
        }
        g_pdp->pal_phos[i] = rgb_to_wm_fast(r, g, b);
    }

    memcpy(g_pdp->M, spacewar_init_mem, sizeof(g_pdp->M));
    g_pdp->PC = SPACEWAR_START_ADDR;

    int sw = 320, sh = 200;
    vga_gfx_probe_screen_size();
    vga_gfx_get_screen_bounds(&sw, &sh);
    if (sw < 320) sw = 320;
    if (sh < 200) sh = 200;

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Spacewar! (PDP-1, 1962)";
    opt.bg_color = 0;
    opt.w = (uint16_t)sw;
    opt.h = (uint16_t)sh;
    g_pdp->wm_win = wm_create_window(&opt);
    if (g_pdp->wm_win) {
        wm_set_fullscreen(g_pdp->wm_win, true);
        wm_set_cursor_visible(g_pdp->wm_win, false);
        wm_compositor_tick();
        if (g_pdp->wm_win->w > 0 && g_pdp->wm_win->h > 0) {
            sw = (int)g_pdp->wm_win->w;
            sh = (int)g_pdp->wm_win->h;
        }
    } else {
        vga_set_mode_13h();
        apply_wm_dac_palette();
        vga_hide_overlay_cursor();
        keyboard_set_app_input_mode(true);
        keyboard_flush_app_queue();
        g_pdp->direct_gfx = true;
    }

    ipo_hd_setup(&g_pdp->hd, g_pdp->wm_win, CRT_DIM, CRT_DIM, true);

    serial_printf("[spacewar] started (PC=0%04o scr=%dx%d pres=(%d,%d %dx%d) k=%d)\n",
                  g_pdp->PC, g_pdp->hd.scr_w, g_pdp->hd.scr_h,
                  g_pdp->hd.pres_x, g_pdp->hd.pres_y, g_pdp->hd.pres_w, g_pdp->hd.pres_h, g_pdp->hd.scale_k);

    uint32_t fps_t0 = timer_millis();
    uint32_t fps_frames = 0;
    uint32_t next_frame_ms = fps_t0;

    while (!g_pdp->quit_req && !g_pdp->halted) {
        poll_inputs(g_pdp);

        for (int cyc = 0; cyc < CYCLES_PER_TICK && !g_pdp->halted; cyc++) {
            pdp1_step(g_pdp);
        }

        render_crt(g_pdp);

        fps_frames++;
        uint32_t now = timer_millis();
        if (now - fps_t0 >= 1000u) {
            serial_printf("[spacewar] %u FPS\n", (fps_frames * 1000u) / (now - fps_t0));
            fps_frames = 0;
            fps_t0 = now;
        }

        next_frame_ms += (fps_frames % 3u == 0u) ? 16u : 17u;
        int32_t rem = (int32_t)(next_frame_ms - timer_millis());
        if (rem < -100) {
            next_frame_ms = timer_millis();
        } else {
            while ((int32_t)(next_frame_ms - timer_millis()) > 0) {
                ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
            }
        }
    }

    if (g_pdp->wm_win && wm_is_window_valid(g_pdp->wm_win)) {
        wm_set_cursor_visible(g_pdp->wm_win, true);
        wm_destroy_window(g_pdp->wm_win);
    } else if (g_pdp->direct_gfx) {
        keyboard_set_app_input_mode(false);
        vga_set_mode_text();
    }
    ipo_hd_free(&g_pdp->hd);
    free(g_pdp);
    return 0;
}
