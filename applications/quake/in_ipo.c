#include "quakedef.h"

#include <stdbool.h>
#include <stdint.h>

#include <driver/input/keyboard.h>
#include <driver/input/mouse.h>
#include <wm.h>

extern wm_window_t *g_quake_wm_win;
extern bool g_quake_direct_gfx;

static int s_quit_pending = 0;
static bool s_ctrl_down = false;
static uint8_t s_prev_btns = 0;
static int64_t s_prev_mx = 0;
static int64_t s_prev_my = 0;
static bool s_mouse_init_pos = false;

int ipo_snd_shutdown_pending(void) {
    return s_quit_pending;
}

static int scancode_to_qkey(uint8_t sc, bool e0) {
    if (e0) {
        switch (sc) {
            case 0x1C: return K_ENTER;
            case 0x1D: return K_CTRL;
            case 0x38: return K_ALT;
            case 0x47: return K_HOME;
            case 0x48: return K_UPARROW;
            case 0x49: return K_PGUP;
            case 0x4B: return K_LEFTARROW;
            case 0x4D: return K_RIGHTARROW;
            case 0x4F: return K_END;
            case 0x50: return K_DOWNARROW;
            case 0x51: return K_PGDN;
            case 0x52: return K_INS;
            case 0x53: return K_DEL;
            default:   return 0;
        }
    }
    switch (sc) {
        case 0x01: return K_ESCAPE;
        case 0x02: return '1'; case 0x03: return '2'; case 0x04: return '3';
        case 0x05: return '4'; case 0x06: return '5'; case 0x07: return '6';
        case 0x08: return '7'; case 0x09: return '8'; case 0x0A: return '9';
        case 0x0B: return '0';
        case 0x0C: return '-'; case 0x0D: return '=';
        case 0x0E: return K_BACKSPACE;
        case 0x0F: return K_TAB;
        case 0x10: return 'q'; case 0x11: return 'w'; case 0x12: return 'e';
        case 0x13: return 'r'; case 0x14: return 't'; case 0x15: return 'y';
        case 0x16: return 'u'; case 0x17: return 'i'; case 0x18: return 'o';
        case 0x19: return 'p'; case 0x1A: return '['; case 0x1B: return ']';
        case 0x1C: return K_ENTER;
        case 0x1D: return K_CTRL;
        case 0x1E: return 'a'; case 0x1F: return 's'; case 0x20: return 'd';
        case 0x21: return 'f'; case 0x22: return 'g'; case 0x23: return 'h';
        case 0x24: return 'j'; case 0x25: return 'k'; case 0x26: return 'l';
        case 0x27: return ';'; case 0x28: return '\'';
        case 0x29: return '~';
        case 0x2A: return K_SHIFT;
        case 0x2B: return '\\';
        case 0x2C: return 'z'; case 0x2D: return 'x'; case 0x2E: return 'c';
        case 0x2F: return 'v'; case 0x30: return 'b'; case 0x31: return 'n';
        case 0x32: return 'm'; case 0x33: return ','; case 0x34: return '.';
        case 0x35: return '/';
        case 0x36: return K_SHIFT;
        case 0x38: return K_ALT;
        case 0x39: return K_SPACE;
        case 0x3B: return K_F1;  case 0x3C: return K_F2;
        case 0x3D: return K_F3;  case 0x3E: return K_F4;
        case 0x3F: return K_F5;  case 0x40: return K_F6;
        case 0x41: return K_F7;  case 0x42: return K_F8;
        case 0x43: return K_F9;  case 0x44: return K_F10;
        case 0x57: return K_F11;
        case 0x58: return K_F12;
        default:   return 0;
    }
}

static void dispatch_qkey(int qk, bool down) {
    if (qk == K_CTRL) s_ctrl_down = down;
    if (down && s_ctrl_down && (qk == 'c' || qk == 'q')) {
        s_quit_pending = 1;
        return;
    }
    if (qk > 0) Key_Event(qk, down);
}

void ipo_in_pump_events(void) {
    if (g_quake_wm_win) {
        if (!wm_is_window_valid(g_quake_wm_win) || !wm_session_active()) {
            s_quit_pending = 1;
            return;
        }
        wm_event_t we;
        static bool s_wm_e0 = false;
        while (wm_poll_event(g_quake_wm_win, &we)) {
            if (we.type == WM_EVENT_CLOSE) {
                s_quit_pending = 1;
            } else if (we.type == WM_EVENT_KEY_EXT) {
                bool rel = (we.data & 0x80000000u) != 0u;
                uint32_t kc = we.data & 0x7FFFFFFFu;
                bool e0 = ((kc & 0xFF00u) == 0xE000u);
                uint8_t sc = (uint8_t)(kc & 0x7Fu);
                int qk = scancode_to_qkey(sc, e0);
                if (qk) dispatch_qkey(qk, !rel);
            } else if (we.type == WM_EVENT_KEY_DOWN || we.type == WM_EVENT_KEY_UP) {
                uint8_t raw = (uint8_t)(we.data & 0xFFu);
                if (raw == 0xE0u) { s_wm_e0 = true; continue; }
                bool rel = (we.type == WM_EVENT_KEY_UP) || ((raw & 0x80u) != 0u);
                int qk = scancode_to_qkey(raw & 0x7Fu, s_wm_e0);
                s_wm_e0 = false;
                if (qk) dispatch_qkey(qk, !rel);
            }
        }
    } else if (g_quake_direct_gfx) {
        static bool s_dir_e0 = false;
        keyboard_poll();
        uint8_t raw;
        while ((raw = keyboard_get_scancode()) != 0) {
            if (raw == 0xE0) { s_dir_e0 = true; continue; }
            bool e0 = s_dir_e0;
            s_dir_e0 = false;
            bool rel = (raw & 0x80u) != 0u;
            uint8_t sc = raw & 0x7Fu;
            if (sc == 0) continue;
            int qk = scancode_to_qkey(sc, e0);
            if (qk) dispatch_qkey(qk, !rel);
        }
    }
}

void IN_Init(void) {
    s_quit_pending = 0;
    s_prev_btns = 0;
    s_mouse_init_pos = false;
}

void IN_Shutdown(void) {}

void IN_Commands(void) {
    mouse_state_t ms;
    mouse_get_state(&ms);
    uint8_t cur = 0;
    if (ms.buttons & MOUSE_BTN_LEFT)   cur |= 1u;
    if (ms.buttons & MOUSE_BTN_RIGHT)  cur |= 2u;
    if (ms.buttons & MOUSE_BTN_MIDDLE) cur |= 4u;

    for (int i = 0; i < 3; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if ((cur & bit) && !(s_prev_btns & bit)) Key_Event(K_MOUSE1 + i, true);
        if (!(cur & bit) && (s_prev_btns & bit)) Key_Event(K_MOUSE1 + i, false);
    }
    s_prev_btns = cur;
}

void IN_Move(usercmd_t *cmd) {
    int64_t cx = 0, cy = 0;
    mouse_get_cumulative_motion(&cx, &cy);
    if (!s_mouse_init_pos) {
        s_prev_mx = cx;
        s_prev_my = cy;
        s_mouse_init_pos = true;
        return;
    }

    float dx = (float)(cx - s_prev_mx) * sensitivity.value;
    float dy = (float)(cy - s_prev_my) * sensitivity.value;
    s_prev_mx = cx;
    s_prev_my = cy;

    if (dx != 0.0f || dy != 0.0f) {
        cl.viewangles[YAW] -= m_yaw.value * dx;
        if ((in_mlook.state & 1) || lookstrafe.value == 0) {
            V_StopPitchDrift();
        }
        if ((in_mlook.state & 1) && !(in_strafe.state & 1)) {
            cl.viewangles[PITCH] += m_pitch.value * dy;
            if (cl.viewangles[PITCH] > 80) cl.viewangles[PITCH] = 80;
            if (cl.viewangles[PITCH] < -70) cl.viewangles[PITCH] = -70;
        } else {
            cmd->forwardmove -= m_forward.value * dy;
        }
    }
}
