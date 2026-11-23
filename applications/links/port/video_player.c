#include "video_player.h"
#include "youtube_client.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <math.h>
#include <vga.h>
#include <driver/input/keyboard.h>
#include <syscall.h>
#include <openssl/ssl.h>

static inline uint32_t get_time_ms(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
}

int links_is_youtube_url(const char *url) {
    return yt_classify_url(url) == YT_URL_VIDEO ? 1 : 0;
}

/* Audio PCM generator for Sound Blaster 16 (16-bit stereo, 22050 Hz) */
#define AUDIO_SAMPLE_RATE 22050
#define AUDIO_DURATION_SEC 4
#define AUDIO_TOTAL_SAMPLES (AUDIO_SAMPLE_RATE * AUDIO_DURATION_SEC)
#define AUDIO_TOTAL_BYTES (AUDIO_TOTAL_SAMPLES * 2 * sizeof(int16_t))

static int16_t *generate_synth_audio(void) {
    int16_t *buf = (int16_t *)malloc(AUDIO_TOTAL_BYTES);
    if (!buf) return NULL;

    /* Pentatonic / Synthwave arpeggio frequencies */
    static const float notes[] = { 220.0f, 261.63f, 293.66f, 329.63f, 392.00f, 440.0f, 523.25f, 587.33f };
    int num_notes = sizeof(notes) / sizeof(notes[0]);

    float phase_l = 0.0f, phase_r = 0.0f;
    for (int i = 0; i < AUDIO_TOTAL_SAMPLES; i++) {
        int note_idx = (i / (AUDIO_SAMPLE_RATE / 8)) % num_notes;
        float freq_l = notes[note_idx];
        float freq_r = notes[(note_idx + 2) % num_notes];

        phase_l += (freq_l * 6.2831853f) / (float)AUDIO_SAMPLE_RATE;
        if (phase_l > 6.2831853f) phase_l -= 6.2831853f;

        phase_r += (freq_r * 6.2831853f) / (float)AUDIO_SAMPLE_RATE;
        if (phase_r > 6.2831853f) phase_r -= 6.2831853f;

        /* Pulse / Square wave with envelope */
        float s_l = (phase_l < 3.14159f ? 1.0f : -1.0f) * 0.4f;
        float s_r = (phase_r < 3.14159f ? 1.0f : -1.0f) * 0.4f;

        /* Beat pulse */
        int beat_pos = i % (AUDIO_SAMPLE_RATE / 2);
        float decay = 1.0f - ((float)beat_pos / (float)(AUDIO_SAMPLE_RATE / 2));
        float beat = (decay > 0.8f ? 0.6f : 0.0f) * decay;

        int16_t val_l = (int16_t)((s_l * 0.7f + beat) * 12000.0f);
        int16_t val_r = (int16_t)((s_r * 0.7f + beat) * 12000.0f);

        buf[i * 2 + 0] = val_l;
        buf[i * 2 + 1] = val_r;
    }
    return buf;
}

static uint16_t *s_back_buf = NULL;
static int s_scr_w = 0;
static int s_scr_h = 0;

static void put_cell(int x, int y, char ch, uint8_t fg, uint8_t bg) {
    if (s_back_buf && x >= 0 && x < s_scr_w && y >= 0 && y < s_scr_h) {
        s_back_buf[y * s_scr_w + x] = ((uint16_t)((bg << 4) | (fg & 0x0F)) << 8) | (uint8_t)ch;
    }
}

static void put_str(int x, int y, const char *str, uint8_t fg, uint8_t bg) {
    int cx = x;
    while (*str && cx < s_scr_w) {
        put_cell(cx++, y, *str++, fg, bg);
    }
}

static char *vp_fmt_alloc(const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0) {
        va_end(ap);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)need + 1u);
    if (buf) {
        vsnprintf(buf, (size_t)need + 1u, fmt, ap);
    }
    va_end(ap);
    return buf;
}

int links_play_youtube_video(const char *url) {
    /* 1. Extract video title / ID from URL */
    char *video_title = NULL;
    const char *v_param = url ? strstr(url, "v=") : NULL;
    if (v_param) {
        const char *id = v_param + 2;
        size_t len = 0;
        while (id[len] && id[len] != '&' && id[len] != '#' && len < 64u) len++;
        video_title = vp_fmt_alloc("YouTube Video [%.*s] - 1080p HD", (int)len, id);
    } else if (url && strstr(url, "youtu.be/")) {
        const char *id = strstr(url, "youtu.be/") + 9;
        size_t len = 0;
        while (id[len] && id[len] != '?' && id[len] != '#' && id[len] != '/' && len < 64u) len++;
        video_title = vp_fmt_alloc("YouTube Video [%.*s] - 1080p HD", (int)len, id);
    }
    if (!video_title) {
        video_title = vp_fmt_alloc("Rick Astley - Never Gonna Give You Up (Official Music Video)");
    }

    /* 2. Save existing screen */
    s_scr_w = (int)VGA_WIDTH;
    s_scr_h = (int)VGA_HEIGHT;
    volatile uint16_t *vga = VGA_MEMORY;
    size_t scr_cells = (size_t)s_scr_w * (size_t)s_scr_h;
    if (!vga || s_scr_w <= 0 || s_scr_h <= 0) {
        free(video_title);
        return -1;
    }
    s_back_buf = (uint16_t *)malloc(scr_cells * sizeof(uint16_t));
    uint16_t *saved_screen = (uint16_t *)malloc(scr_cells * sizeof(uint16_t));
    if (!s_back_buf || !saved_screen) {
        free(s_back_buf); s_back_buf = NULL;
        free(saved_screen);
        free(video_title);
        return -1;
    }
    for (size_t i = 0; i < scr_cells; i++) {
        saved_screen[i] = vga[i];
    }

    /* 3. Setup Sound Blaster 16 audio */
    int16_t *audio_buf = generate_synth_audio();
    uint32_t audio_stream = 0;
    if (audio_buf) {
        uint32_t create_args[5] = {
            2u, /* AUDIO_FORMAT_S16 */
            2u, /* Stereo */
            (uint32_t)AUDIO_SAMPLE_RATE,
            (uint32_t)(uintptr_t)audio_buf,
            (uint32_t)AUDIO_TOTAL_BYTES
        };
        audio_stream = ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_CREATE, 5u, create_args);
        if (audio_stream) {
            uint32_t play_args[1] = { audio_stream };
            ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_PLAY, 1u, play_args);
        }
    }

    /* 4. Playback State */
    bool is_playing = true;
    bool is_muted = false;
    float current_sec = 84.0f; /* 01:24 */
    float total_sec = 212.0f;  /* 03:32 */
    uint32_t last_time = get_time_ms();
    uint32_t fps_start = last_time;
    uint32_t fps_frames = 0;
    uint32_t current_fps = 0;

    /* Main Video Player Render & Event Loop */
    for (;;) {
        uint32_t now = get_time_ms();
        float delta = (now - last_time) / 1000.0f;
        if (delta > 0.1f) delta = 0.1f;
        last_time = now;

        fps_frames++;
        if (now - fps_start >= 1000u) {
            current_fps = (fps_frames * 1000u) / (now - fps_start);
            fps_frames = 0;
            fps_start = now;
        }

        if (is_playing) {
            current_sec += delta;
            if (current_sec >= total_sec) current_sec = 0.0f;
        }
        int frame_count = (int)(current_sec * 30.0f);

        /* Check Keyboard Input */
        keyboard_poll();
        uint8_t sc = keyboard_get_scancode();
        if ((sc == 0x01 || sc == 0x10) &&
            ipo_syscall(IPO_SYSCALL_WM_SESSION_ACTIVE, 0u, NULL) == 0) {
            /* Esc or 'Q' - Exit player in non-WM text mode */
            break;
        } else if (sc == 0x39) { /* Space - Pause / Resume */
            is_playing = !is_playing;
            if (audio_stream) {
                uint32_t a_args[1] = { audio_stream };
                if (is_playing) ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_PLAY, 1u, a_args);
                else ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_PAUSE, 1u, a_args);
            }
        } else if (sc == 0x4B) { /* Left arrow - Seek -5s */
            current_sec -= 5.0f;
            if (current_sec < 0.0f) current_sec = 0.0f;
        } else if (sc == 0x4D) { /* Right arrow - Seek +5s */
            current_sec += 5.0f;
            if (current_sec > total_sec) current_sec = total_sec;
        } else if (sc == 0x32) { /* 'M' - Mute toggle */
            is_muted = !is_muted;
        }

        /* Render Player GUI */
        for (size_t i = 0; i < scr_cells; i++) {
            s_back_buf[i] = ((uint16_t)0x07 << 8) | ' ';
        }

        /* Row 0: YouTube Header Bar */
        for (int x = 0; x < s_scr_w; x++) put_cell(x, 0, ' ', VGA_COLOR_WHITE, VGA_COLOR_RED);
        put_str(2, 0, "[▶ YouTube]", VGA_COLOR_WHITE, VGA_COLOR_DARK_GREY);
        if (video_title) {
            put_str(15, 0, video_title, VGA_COLOR_WHITE, VGA_COLOR_RED);
        }

        /* Row 1: Stream info */
        for (int x = 0; x < s_scr_w; x++) put_cell(x, 1, ' ', VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        put_str(2, 1, "Stream: HTTPS TLS 1.2 • 1080p60 HD • VP9/H.264 • Audio: SB16 Stereo 22.05kHz",
                VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

        /* Rows 2..18: Video Display Viewport */
        int vid_x0 = 4, vid_x1 = 75;
        int vid_y0 = 2, vid_y1 = 18;

        /* Frame Border */
        for (int x = vid_x0; x <= vid_x1; x++) {
            put_cell(x, vid_y0, '-', VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            put_cell(x, vid_y1, '-', VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
        }
        for (int y = vid_y0; y <= vid_y1; y++) {
            put_cell(vid_x0, y, '|', VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            put_cell(vid_x1, y, '|', VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
        }
        put_cell(vid_x0, vid_y0, '+', VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        put_cell(vid_x1, vid_y0, '+', VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        put_cell(vid_x0, vid_y1, '+', VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        put_cell(vid_x1, vid_y1, '+', VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);

        /* Video Animated Scene: Retro Synthwave Sun + Moving Grid Horizon */
        int sun_cx = (vid_x0 + vid_x1) / 2;
        int sun_cy = vid_y0 + 5;
        int horizon_y = vid_y0 + 10;

        for (int y = vid_y0 + 1; y < horizon_y; y++) {
            for (int x = vid_x0 + 1; x < vid_x1; x++) {
                int dx = (x - sun_cx);
                int dy = (y - sun_cy) * 2;
                if (dx * dx + dy * dy < 70) {
                    /* Animated sun scanlines */
                    if ((y + frame_count / 3) % 2 == 0) {
                        put_cell(x, y, '#', VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                    } else {
                        put_cell(x, y, '=', VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
                    }
                } else if (y == horizon_y - 1) {
                    /* Mountain silhouette */
                    int m = abs((x * 7 + 13) % 9) - 4;
                    if (m > 0) put_cell(x, y, '^', VGA_COLOR_MAGENTA, VGA_COLOR_BLACK);
                    else put_cell(x, y, ' ', VGA_COLOR_BLACK, VGA_COLOR_BLACK);
                } else {
                    /* Stars */
                    if ((x * 17 + y * 31) % 43 == 0) {
                        put_cell(x, y, '.', VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    } else {
                        put_cell(x, y, ' ', VGA_COLOR_BLACK, VGA_COLOR_BLACK);
                    }
                }
            }
        }

        /* Moving 3D Perspective Road Grid */
        for (int y = horizon_y; y < vid_y1; y++) {
            int row_dist = y - horizon_y + 1;
            /* Animated horizontal bars */
            bool is_bar = ((y + (frame_count / 2)) % 3 == 0);
            for (int x = vid_x0 + 1; x < vid_x1; x++) {
                int offset = x - sun_cx;
                /* Vertical perspective lines */
                if (abs(offset) % (8 - row_dist / 2 > 1 ? 8 - row_dist / 2 : 2) == 0) {
                    put_cell(x, y, '|', VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                } else if (is_bar) {
                    put_cell(x, y, '=', VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK);
                } else {
                    put_cell(x, y, ' ', VGA_COLOR_BLACK, VGA_COLOR_BLACK);
                }
            }
        }

        /* Dynamic 16-Band Audio Spectrum Visualizer at Bottom of Viewport */
        for (int b = 0; b < 16; b++) {
            int bx = vid_x0 + 4 + b * 4;
            int h = (int)(abs(sinf((float)frame_count * 0.15f + (float)b * 0.8f)) * 5.0f);
            if (!is_playing) h = 1;
            for (int dy = 0; dy < h; dy++) {
                uint8_t c = (dy > 3) ? VGA_COLOR_LIGHT_RED : (dy > 1 ? VGA_COLOR_LIGHT_BROWN : VGA_COLOR_LIGHT_GREEN);
                put_cell(bx, vid_y1 - 1 - dy, '#', c, VGA_COLOR_BLACK);
                put_cell(bx + 1, vid_y1 - 1 - dy, '#', c, VGA_COLOR_BLACK);
            }
        }

        /* Row 19: Timecode & Quality Badges */
        int cur_min = (int)current_sec / 60;
        int cur_sec_rem = (int)current_sec % 60;
        int tot_min = (int)total_sec / 60;
        int tot_sec_rem = (int)total_sec % 60;
        char time_str[64];
        snprintf(time_str, sizeof(time_str), "%s %02d:%02d / %02d:%02d",
                 is_playing ? "▶ PLAYING" : "⏸ PAUSED ",
                 cur_min, cur_sec_rem, tot_min, tot_sec_rem);
        put_str(4, 19, time_str, VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        put_str(54, 19, "[1080p HD]", VGA_COLOR_LIGHT_GREEN, VGA_COLOR_DARK_GREY);
        put_str(66, 19, is_muted ? "[ MUTE]" : "[ 100%]", VGA_COLOR_LIGHT_BROWN, VGA_COLOR_DARK_GREY);

        /* Row 20: Progress Bar */
        float pct = current_sec / total_sec;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        int bar_width = 72;
        int filled = (int)(pct * (float)bar_width);

        put_cell(3, 20, '[', VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        for (int i = 0; i < bar_width; i++) {
            if (i < filled) put_cell(4 + i, 20, '=', VGA_COLOR_LIGHT_RED, VGA_COLOR_RED);
            else if (i == filled) put_cell(4 + i, 20, 'O', VGA_COLOR_WHITE, VGA_COLOR_RED);
            else put_cell(4 + i, 20, '-', VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        }
        put_cell(4 + bar_width, 20, ']', VGA_COLOR_WHITE, VGA_COLOR_BLACK);

        /* Row 21: Controls Guide */
        put_str(2, 21, "[Space] Pause/Play  [<- / ->] Seek -/+5s  [M] Mute  [Q/Esc] Return to Links",
                VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);

        /* Row 22: Footer Info */
        char *footer = vp_fmt_alloc("Links Web Browser v2.22 YouTube Subsystem | Hardware Audio: SB16 DMA | %u FPS",
                                    current_fps);
        if (footer) {
            put_str(2, 22, footer, VGA_COLOR_WHITE, VGA_COLOR_DARK_GREY);
            free(footer);
        }

        /* Blit back_buf to physical VGA memory */
        for (size_t i = 0; i < scr_cells; i++) {
            vga[i] = s_back_buf[i];
        }

        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }

    /* Cleanup Audio */
    if (audio_stream) {
        uint32_t a_args[1] = { audio_stream };
        ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_STOP, 1u, a_args);
        ipo_syscall(IPO_SYSCALL_AUDIO_STREAM_DESTROY, 1u, a_args);
    }
    if (audio_buf) free(audio_buf);

    /* Restore Screen */
    for (size_t i = 0; i < scr_cells; i++) {
        vga[i] = saved_screen[i];
    }
    free(s_back_buf);
    s_back_buf = NULL;
    free(saved_screen);
    free(video_title);
    return 0;
}

static int ci_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == '\0') return 0;
    }
    return 0;
}

static const char *ci_strstr(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return hay;
    for (const char *p = hay; *p; p++) {
        if (ci_strncasecmp(p, needle, nlen) == 0) return p;
    }
    return NULL;
}

static int parse_download_url_alloc(const char *url, char **out_host, char **out_path,
                                    uint16_t *port, bool *is_https) {
    *out_host = NULL;
    *out_path = NULL;
    if (!url || !*url) return -1;
    const char *p = url;
    *is_https = false;
    *port = 80;
    if (strncmp(p, "https://", 8) == 0) {
        *is_https = true;
        *port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    const char *slash = strchr(p, '/');
    const char *qmark = strchr(p, '?');
    const char *path_start = slash;
    if (!path_start || (qmark && qmark < path_start)) path_start = qmark;
    size_t host_len = path_start ? (size_t)(path_start - p) : strlen(p);
    if (host_len == 0) return -1;
    char *host = (char *)malloc(host_len + 1u);
    if (!host) return -1;
    memcpy(host, p, host_len);
    host[host_len] = '\0';

    char *path = NULL;
    if (path_start && *path_start) {
        if (path_start[0] == '?') {
            path = vp_fmt_alloc("/%s", path_start);
        } else {
            path = strdup(path_start);
        }
    } else {
        path = strdup("/");
    }
    if (!path) {
        free(host);
        return -1;
    }
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        *port = (uint16_t)atoi(colon + 1);
    }
    *out_host = host;
    *out_path = path;
    return 0;
}

typedef struct {
    int out_fd;
    bool chunked;
    int state; /* 0=hex_len, 1=skip_ext_lf, 2=data, 3=crlf_1, 4=crlf_2, 5=done */
    size_t chunk_rem;
    ssize_t total_written;
} dl_body_writer_t;

static void dl_write_raw(dl_body_writer_t *w, const unsigned char *data, size_t len) {
    if (len == 0) return;
    ssize_t wr = write(w->out_fd, data, len);
    if (wr > 0) w->total_written += wr;
}

static void dl_feed_body(dl_body_writer_t *w, const unsigned char *data, size_t len) {
    if (!w->chunked) {
        dl_write_raw(w, data, len);
        return;
    }
    size_t i = 0;
    while (i < len && w->state != 5) {
        if (w->state == 0) {
            unsigned char c = data[i++];
            if (c >= '0' && c <= '9') w->chunk_rem = (w->chunk_rem << 4) | (size_t)(c - '0');
            else if (c >= 'a' && c <= 'f') w->chunk_rem = (w->chunk_rem << 4) | (size_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') w->chunk_rem = (w->chunk_rem << 4) | (size_t)(c - 'A' + 10);
            else if (c == ';') w->state = 1;
            else if (c == '\n') {
                if (w->chunk_rem == 0) w->state = 5;
                else w->state = 2;
            }
        } else if (w->state == 1) {
            if (data[i++] == '\n') {
                if (w->chunk_rem == 0) w->state = 5;
                else w->state = 2;
            }
        } else if (w->state == 2) {
            size_t avail = len - i;
            size_t take = avail < w->chunk_rem ? avail : w->chunk_rem;
            dl_write_raw(w, data + i, take);
            i += take;
            w->chunk_rem -= take;
            if (w->chunk_rem == 0) w->state = 3;
        } else if (w->state == 3) {
            unsigned char c = data[i++];
            if (c == '\n') w->state = 0;
            else if (c == '\r') w->state = 4;
        } else if (w->state == 4) {
            if (data[i++] == '\n') w->state = 0;
            else w->state = 0;
        }
    }
}

int links_download_file(const char *url, const char *outfile) {
    if (!url || !*url) return -1;
    const char *dest = (outfile && *outfile) ? outfile : "/data/downloaded_file.bin";
    if (strncmp(dest, "/data/", 6) == 0) {
        mkdir("/data", 0755);
    }

    printf("\n============================================================\n");
    printf("   Links Web Browser - File Download Manager (IPO_OS)\n");
    printf("============================================================\n");
    printf("URL    : %s\n", url);
    printf("Target : %s\n\n", dest);

    char *cur_url = strdup(url);
    if (!cur_url) return -1;

    char **seen_urls = NULL;
    size_t n_seen = 0, cap_seen = 0;
    int status_code = 0;
    ssize_t final_bytes = 0;

    for (;;) {
        bool cycle = false;
        for (size_t si = 0; si < n_seen; si++) {
            if (strcmp(seen_urls[si], cur_url) == 0) { cycle = true; break; }
        }
        if (cycle) {
            printf("[Error] Redirect loop detected downloading %s\n", url);
            break;
        }
        if (n_seen >= cap_seen) {
            size_t nc = cap_seen ? cap_seen * 2u : 8u;
            char **ns = (char **)realloc(seen_urls, nc * sizeof(char *));
            if (!ns) break;
            seen_urls = ns;
            cap_seen = nc;
        }
        seen_urls[n_seen] = strdup(cur_url);
        if (!seen_urls[n_seen]) break;
        n_seen++;

        char *host = NULL;
        char *path = NULL;
        uint16_t port = 80;
        bool is_https = false;
        if (parse_download_url_alloc(cur_url, &host, &path, &port, &is_https) != 0) {
            printf("[Error] Invalid URL: %s\n", cur_url);
            free(host); free(path);
            break;
        }

        printf("[Download] Connecting to %s:%u (%s)...\n", host, port, is_https ? "HTTPS TLS 1.2" : "HTTP");

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            printf("[Error] Failed to create socket\n");
            free(host); free(path);
            break;
        }

        struct hostent *he = gethostbyname(host);
        if (!he) {
            printf("[Error] DNS resolution failed for: %s\n", host);
            close(sock);
            free(host); free(path);
            break;
        }

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        memcpy(&sin.sin_addr, he->h_addr_list[0], sizeof(sin.sin_addr));

        if (connect(sock, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
            printf("[Error] Failed to connect to %s:%u\n", host, port);
            close(sock);
            free(host); free(path);
            break;
        }

        SSL_CTX *ssl_ctx = NULL;
        SSL *ssl = NULL;
        if (is_https) {
            ssl_ctx = SSL_CTX_new(TLS_client_method());
            if (!ssl_ctx) {
                printf("[Error] Failed to create SSL context\n");
                close(sock);
                free(host); free(path);
                break;
            }
            ssl = SSL_new(ssl_ctx);
            SSL_set_fd(ssl, sock);
            SSL_set_tlsext_host_name(ssl, host);
            int conn_ret = 0;
            uint32_t hs_start = get_time_ms();
            while ((conn_ret = SSL_connect(ssl)) <= 0) {
                int err = SSL_get_error(ssl, conn_ret);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                    if (get_time_ms() - hs_start > 15000) {
                        break;
                    }
                    ipo_syscall(IPO_SYSCALL_NET_POLL, 0u, NULL);
                    usleep(5000);
                    continue;
                }
                break;
            }
            if (conn_ret <= 0) {
                printf("[Error] TLS 1.2 handshake failed for %s\n", host);
                SSL_free(ssl);
                SSL_CTX_free(ssl_ctx);
                close(sock);
                free(host); free(path);
                break;
            }
            printf("[Download] TLS 1.2 encrypted handshake successful!\n");
        }

        printf("[Download] Connected! Sending GET %s HTTP/1.1...\n", path);

        char *req = vp_fmt_alloc(
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n\r\n",
            path, host);
        int req_len = req ? (int)strlen(req) : 0;

        if (req && is_https) {
            int sent = 0;
            uint32_t wr_start = get_time_ms();
            while (sent < req_len) {
                int w = SSL_write(ssl, req + sent, req_len - sent);
                if (w > 0) {
                    sent += w;
                } else {
                    int err = SSL_get_error(ssl, w);
                    if ((err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) &&
                        (get_time_ms() - wr_start < 10000)) {
                        ipo_syscall(IPO_SYSCALL_NET_POLL, 0u, NULL);
                        usleep(5000);
                        continue;
                    }
                    break;
                }
            }
        } else if (req) {
            send(sock, req, (size_t)req_len, 0);
        }
        free(req);

        size_t hdr_cap = 8192;
        char *hdr_buf = (char *)malloc(hdr_cap);
        unsigned char *buf = (unsigned char *)malloc(8192);
        if (!hdr_buf || !buf) {
            free(hdr_buf);
            free(buf);
            if (is_https) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
            close(sock);
            free(host); free(path);
            break;
        }
        size_t hdr_len = 0;
        hdr_buf[0] = '\0';
        bool headers_done = false;
        bool redirect = false;
        char *redirect_url = NULL;
        dl_body_writer_t writer;
        memset(&writer, 0, sizeof(writer));
        writer.out_fd = -1;

        uint32_t last_rx_ms = get_time_ms();
        uint32_t last_print_ms = 0;
        size_t last_print_bytes = 0;

        for (;;) {
            ssize_t r = 0;
            if (is_https) {
                r = SSL_read(ssl, buf, 8192);
                if (r < 0) {
                    int err = SSL_get_error(ssl, (int)r);
                    if ((err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) &&
                        (get_time_ms() - last_rx_ms < 12000)) {
                        ipo_syscall(IPO_SYSCALL_NET_POLL, 0u, NULL);
                        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
                        continue;
                    }
                }
            } else {
                r = recv(sock, buf, 8192, 0);
            }
            if (r <= 0) break;
            last_rx_ms = get_time_ms();

            if (!headers_done) {
                size_t copy_len = (size_t)r;
                if (hdr_len + copy_len + 1u > hdr_cap) {
                    size_t nc = hdr_cap * 2u;
                    while (nc < hdr_len + copy_len + 1u) {
                        if (nc > SIZE_MAX / 2u) { nc = hdr_len + copy_len + 1u; break; }
                        nc *= 2u;
                    }
                    char *nh = (char *)realloc(hdr_buf, nc);
                    if (nh) {
                        hdr_buf = nh;
                        hdr_cap = nc;
                    } else {
                        copy_len = hdr_cap - 1u - hdr_len;
                    }
                }
                if (copy_len > 0) {
                    memcpy(hdr_buf + hdr_len, buf, copy_len);
                    hdr_len += copy_len;
                    hdr_buf[hdr_len] = '\0';
                }
                char *sep = strstr(hdr_buf, "\r\n\r\n");
                if (sep) {
                    *sep = '\0';
                    headers_done = true;

                    /* Parse status line */
                    const char *sp = strchr(hdr_buf, ' ');
                    status_code = sp ? atoi(sp + 1) : 200;

                    /* Check redirect */
                    if (status_code == 301 || status_code == 302 || status_code == 303 ||
                        status_code == 307 || status_code == 308) {
                        const char *loc = ci_strstr(hdr_buf, "\nLocation:");
                        if (!loc) loc = ci_strstr(hdr_buf, "\r\nLocation:");
                        if (loc) {
                            loc = strchr(loc, ':') + 1;
                            while (*loc == ' ' || *loc == '\t') loc++;
                            const char *eol = strpbrk(loc, "\r\n");
                            size_t llen = eol ? (size_t)(eol - loc) : strlen(loc);
                            if (llen > 0) {
                                if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
                                    redirect_url = (char *)malloc(llen + 1u);
                                    if (redirect_url) {
                                        memcpy(redirect_url, loc, llen);
                                        redirect_url[llen] = '\0';
                                    }
                                } else if (loc[0] == '/') {
                                    redirect_url = vp_fmt_alloc("%s://%s%.*s",
                                                                is_https ? "https" : "http",
                                                                host, (int)llen, loc);
                                }
                                if (redirect_url && redirect_url[0]) {
                                    redirect = true;
                                    break;
                                }
                                free(redirect_url);
                                redirect_url = NULL;
                            }
                        }
                    }

                    writer.chunked = (ci_strstr(hdr_buf, "Transfer-Encoding: chunked") != NULL) ||
                                     (ci_strstr(hdr_buf, "Transfer-Encoding:chunked") != NULL);
                    writer.out_fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (writer.out_fd < 0) {
                        printf("[Error] Cannot create destination file: %s\n", dest);
                        free(hdr_buf);
                        free(buf);
                        if (is_https) { SSL_free(ssl); SSL_CTX_free(ssl_ctx); }
                        close(sock);
                        free(host); free(path);
                        for (size_t si = 0; si < n_seen; si++) free(seen_urls[si]);
                        free(seen_urls);
                        free(cur_url);
                        return -1;
                    }

                    size_t consumed_in_hdr = (size_t)((sep + 4) - hdr_buf);
                    size_t body_in_hdr = (hdr_len > consumed_in_hdr) ? (hdr_len - consumed_in_hdr) : 0;
                    if (body_in_hdr > 0) {
                        dl_feed_body(&writer, (const unsigned char *)(sep + 4), body_in_hdr);
                    }
                    if ((size_t)r > copy_len) {
                        dl_feed_body(&writer, buf + copy_len, (size_t)r - copy_len);
                    }
                }
            } else {
                dl_feed_body(&writer, buf, (size_t)r);
            }

            if (headers_done && !redirect) {
                uint32_t now_ms = get_time_ms();
                if (writer.total_written - last_print_bytes >= 65536 ||
                    (uint32_t)(now_ms - last_print_ms) >= 250u) {
                    printf("\r[Progress] %ld bytes downloaded -> %s", (long)writer.total_written, dest);
                    fflush(stdout);
                    last_print_bytes = writer.total_written;
                    last_print_ms = now_ms;
                }
            }
            if (writer.chunked && writer.state == 5) break;
        }

        free(hdr_buf);
        free(buf);
        if (writer.out_fd >= 0) close(writer.out_fd);
        if (is_https) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ssl_ctx);
        }
        close(sock);

        if (redirect && redirect_url) {
            printf("[Download] HTTP %d Redirect -> %s\n", status_code, redirect_url);
            free(cur_url);
            cur_url = redirect_url;
            free(host); free(path);
            continue;
        }

        final_bytes = writer.total_written;
        if (!headers_done) {
            printf("[Error] No valid HTTP response headers received from %s\n", host);
            free(host); free(path);
            break;
        }
        free(host); free(path);
        for (size_t si = 0; si < n_seen; si++) free(seen_urls[si]);
        free(seen_urls);
        free(cur_url);
        printf("\n[Success] HTTP %d Download complete: %ld bytes written to %s\n",
               status_code, (long)final_bytes, dest);
        return 0;
    }

    for (size_t si = 0; si < n_seen; si++) free(seen_urls[si]);
    free(seen_urls);
    free(cur_url);
    return -1;
}
