/**
 * fps_meter.c — Real-Time FPS and System Performance Monitor for IPO_OS
 *
 * Displays live frame rate, frame render latencies, rolling histogram,
 * active window count, and system uptime in a dedicated WM desktop window.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <wm.h>
#include <syscall.h>
#include <system/state.h>
#include <system/timer.h>

static void fps_draw_fmt(uint8_t *fb, uint16_t cur_w, uint16_t cur_h, int x, int y, uint8_t col,
                         const char *fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    int len = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (len < 0) {
        va_end(ap);
        return;
    }
    char *buf = (char *)malloc((size_t)len + 1u);
    if (!buf) {
        va_end(ap);
        return;
    }
    vsnprintf(buf, (size_t)len + 1u, fmt, ap);
    va_end(ap);
    wm_buf_draw_string(fb, cur_w, cur_h, x, y, buf, col);
    free(buf);
}

#define WIN_W 220
#define WIN_H 120
#define HIST_LEN 96

#define CLR_BLACK         0
#define CLR_BLUE          1
#define CLR_GREEN         2
#define CLR_CYAN          3
#define CLR_RED           4
#define CLR_MAGENTA       5
#define CLR_BROWN         6
#define CLR_LIGHT_GRAY    7
#define CLR_DARK_GRAY     8
#define CLR_LIGHT_BLUE    9
#define CLR_LIGHT_GREEN  10
#define CLR_LIGHT_CYAN   11
#define CLR_LIGHT_RED    12
#define CLR_LIGHT_MAGENTA 13
#define CLR_YELLOW       14
#define CLR_WHITE        15

static volatile bool running = true;
static volatile bool reset_requested = false;

static void on_window_event(wm_window_t *w, uint32_t event, uint32_t data) {
    (void)w;
    if (event == WM_EVENT_CLOSE) {
        running = false;
    } else if (event == WM_EVENT_KEY_DOWN) {
        uint8_t sc = (uint8_t)(data & 0x7F);
        if (sc == 19) { /* 'R' key */
            reset_requested = true;
        }
    } else if (event == WM_EVENT_CLICK) {
        /* Click anywhere to reset stats */
        reset_requested = true;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("[fps_meter] Launching System Performance Monitor...\n");

    wm_window_options_t opt = WM_WINDOW_OPTIONS_DEFAULT;
    opt.title = "Performance Monitor";
    opt.x = 90;
    opt.y = 35;
    opt.w = WIN_W;
    opt.h = WIN_H;

    wm_window_t *win = wm_create_window(&opt);
    if (!win) {
        printf("[fps_meter] Failed to create window\n");
        return 1;
    }

    int hist_cap = HIST_LEN;
    uint16_t *history = (uint16_t *)calloc((size_t)hist_cap, sizeof(uint16_t));
    if (!history) {
        wm_destroy_window(win);
        return 1;
    }
    int hist_head = 0;

    uint32_t frame_count = 0;
    uint32_t window_frames = 0;
    uint32_t window_start_time = timer_millis();
    uint32_t stats_start_time = timer_millis();
    uint32_t last_frame_time = timer_millis();

    float cur_fps = 0.0f;
    float avg_fps = 0.0f;
    float min_fps = 999.0f;
    float max_fps = 0.0f;
    uint32_t cur_dt = 1;

    while (running && wm_session_active() && !system_is_interrupted() && wm_is_window_valid(win)) {
        wm_event_t ev;
        while (wm_poll_event(win, &ev)) {
            on_window_event(win, ev.type, ev.data);
        }
        if (!running || !wm_is_window_valid(win)) break;

        uint32_t loop_start = timer_millis();

        /* Calculate delta time */
        cur_dt = loop_start - last_frame_time;
        last_frame_time = loop_start;

        /* Add to history */
        history[hist_head] = (uint16_t)(cur_dt > 0 ? cur_dt : 1);
        hist_head = (hist_head + 1) % hist_cap;

        /* Calculate stats */
        frame_count++;
        window_frames++;
        uint32_t win_elapsed = loop_start - window_start_time;
        if (win_elapsed >= 100) {
            cur_fps = ((float)window_frames * 1000.0f) / (float)win_elapsed;
            window_frames = 0;
            window_start_time = loop_start;
        } else if (cur_fps == 0.0f && cur_dt > 0) {
            cur_fps = 1000.0f / (float)cur_dt;
        }
        if (cur_fps > 9999.0f) cur_fps = 9999.0f;

        uint32_t total_elapsed = loop_start - stats_start_time;
        if (total_elapsed >= 500) {
            avg_fps = ((float)frame_count * 1000.0f) / (float)total_elapsed;
            if (frame_count > 5) {
                if (cur_fps < min_fps) min_fps = cur_fps;
                if (cur_fps > max_fps) max_fps = cur_fps;
            }
        } else {
            avg_fps = cur_fps;
        }

        if (reset_requested) {
            reset_requested = false;
            frame_count = 0;
            window_frames = 0;
            window_start_time = loop_start;
            stats_start_time = loop_start;
            min_fps = cur_fps;
            max_fps = cur_fps;
            avg_fps = cur_fps;
            memset(history, 0, (size_t)hist_cap * sizeof(uint16_t));
        }

        /* Render UI */
        uint8_t *fb = win->framebuf;
        if (fb) {
            int cur_w = win->w;
            int cur_h = win->h;

            /* Background: dark theme */
            wm_buf_fill_rect(fb, cur_w, cur_h, 0, 0, cur_w, cur_h, CLR_BLACK);

            /* Top info bar */
            uint8_t fps_col = (cur_fps >= 28.0f) ? CLR_LIGHT_GREEN : ((cur_fps >= 18.0f) ? CLR_YELLOW : CLR_LIGHT_RED);

            fps_draw_fmt(fb, cur_w, cur_h, 8, 4, fps_col, "FPS: %d.%d", (int)cur_fps, (int)(cur_fps * 10.0f) % 10);
            fps_draw_fmt(fb, cur_w, cur_h, 95, 4, CLR_LIGHT_CYAN, "dt: %u ms", (unsigned int)cur_dt);
            fps_draw_fmt(fb, cur_w, cur_h, 155, 4, CLR_WHITE, "Avg: %d.%d", (int)avg_fps, (int)(avg_fps * 10.0f) % 10);

            /* Line 2: Min / Max / Windows count */
            int win_cnt = wm_get_window_count();
            fps_draw_fmt(fb, cur_w, cur_h, 8, 14, CLR_LIGHT_GRAY, "Min: %d.%d", (int)min_fps, (int)(min_fps * 10.0f) % 10);
            fps_draw_fmt(fb, cur_w, cur_h, 85, 14, CLR_LIGHT_GRAY, "Max: %d.%d", (int)max_fps, (int)(max_fps * 10.0f) % 10);
            fps_draw_fmt(fb, cur_w, cur_h, 160, 14, CLR_LIGHT_MAGENTA, "Wins: %d", win_cnt);

            /* Divider line */
            wm_buf_draw_line(fb, cur_w, cur_h, 6, 24, cur_w - 7, 24, CLR_DARK_GRAY);

            /* Performance Histogram Box */
            int gx = 10, gy = 27;
            int gw = cur_w - 20;
            int gh = cur_h - 55;
            if (gw < 20) gw = 20;
            if (gh < 15) gh = 15;

            wm_buf_fill_rect(fb, cur_w, cur_h, gx, gy, gw, gh, 23); /* Very dark navy */
            wm_buf_draw_rect(fb, cur_w, cur_h, gx - 1, gy - 1, gw + 2, gh + 2, CLR_DARK_GRAY);

            /* Target reference lines:
             * 33 ms (30 FPS) -> y = (gy + gh) - (33 * gh / 70)
             * 16 ms (60 FPS) -> y = (gy + gh) - (16 * gh / 70)
             */
            int y_30fps = gy + gh - (33 * gh / 70);
            int y_60fps = gy + gh - (16 * gh / 70);

            /* 60 FPS reference line (dotted) */
            if (y_60fps > gy && y_60fps < gy + gh) {
                for (int lx = gx; lx < gx + gw; lx += 4) {
                    wm_buf_put_pixel(fb, cur_w, cur_h, lx, y_60fps, CLR_DARK_GRAY);
                }
                wm_buf_draw_string(fb, cur_w, cur_h, gx + 2, y_60fps - 7, "60fps", CLR_LIGHT_GRAY);
            }

            /* 30 FPS reference line (dotted) */
            if (y_30fps > gy && y_30fps < gy + gh) {
                for (int lx = gx; lx < gx + gw; lx += 4) {
                    wm_buf_put_pixel(fb, cur_w, cur_h, lx, y_30fps, CLR_LIGHT_BLUE);
                }
                wm_buf_draw_string(fb, cur_w, cur_h, gx + 2, y_30fps - 7, "30fps", CLR_LIGHT_BLUE);
            }

            /* Draw history bars, expanding ring capacity when resized wider */
            int bar_w = 2;
            int num_bars = gw / bar_w;
            if (num_bars > hist_cap) {
                int new_cap = hist_cap;
                while (new_cap < num_bars) new_cap *= 2;
                uint16_t *nh = (uint16_t *)calloc((size_t)new_cap, sizeof(uint16_t));
                if (nh) {
                    int pad = new_cap - hist_cap;
                    for (int k = 0; k < hist_cap; k++) {
                        nh[pad + k] = history[(hist_head + k) % hist_cap];
                    }
                    free(history);
                    history = nh;
                    hist_cap = new_cap;
                    hist_head = 0;
                } else {
                    num_bars = hist_cap;
                }
            }

            for (int i = 0; i < num_bars; i++) {
                int idx = (hist_head - num_bars + i + hist_cap) % hist_cap;
                uint16_t dt_val = history[idx];
                if (dt_val == 0) continue;

                int bar_h = (int)((dt_val * (gh - 2)) / 70);
                if (bar_h < 1) bar_h = 1;
                if (bar_h > gh - 2) bar_h = gh - 2;

                uint8_t bar_col = CLR_LIGHT_GREEN;
                if (dt_val > 50) bar_col = CLR_LIGHT_RED;
                else if (dt_val > 34) bar_col = CLR_YELLOW;

                int bx = gx + i * bar_w;
                int by = gy + gh - 1 - bar_h;
                wm_buf_fill_rect(fb, cur_w, cur_h, bx, by, bar_w - 1, bar_h, bar_col);
            }

            /* Bottom status / controls */
            int bot_y = cur_h - 18;
            if (bot_y > gy + gh) {
                wm_buf_draw_line(fb, cur_w, cur_h, 6, bot_y, cur_w - 7, bot_y, CLR_DARK_GRAY);
                uint32_t uptime = timer_seconds();
                fps_draw_fmt(fb, cur_w, cur_h, 8, bot_y + 4, CLR_LIGHT_GRAY, "Uptime: %02u:%02u", uptime / 60, uptime % 60);
                wm_buf_draw_string(fb, cur_w, cur_h, cur_w > 200 ? cur_w - 60 : 85, bot_y + 4, "[R]eset", CLR_WHITE);
            }

            wm_invalidate(win);
        }

        /* Uncapped frame processing: cooperative single yield per frame */
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    }

    free(history);
    wm_destroy_window(win);
    printf("[fps_meter] Performance Monitor closed.\n");
    return 0;
}
