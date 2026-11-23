#include "mp4_player.h"
#include "youtube_client.h"

#include <glib.h>
#include "mp4.h"
#include "aac.h"
#include "oh264_wrap.h"

#include <driver/audio_core.h>
#include <ipo/audio.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define YT_ANDROID_UA "com.google.android.youtube/20.10.38 (Linux; U; Android 14) gzip"

typedef enum {
    MP4_ST_IDLE = 0,
    MP4_ST_YT_RESOLVE,
    MP4_ST_STREAM,
    MP4_ST_ERROR
} mp4_state_t;

typedef struct {
    ns_mp4_sample smp;
    uint32_t pts_ms;
} mp4_vq_item_t;

struct mp4_player {
    mp4_state_t state;
    char *input_url;
    char *video_id;
    char *stream_url;
    char *stream_ua;
    char *title;
    char *author;
    char *status_text;

    yt_http_stream_t http;

    /* Innertube player JSON buffer */
    char *yt_json;
    size_t yt_json_len;
    size_t yt_json_cap;

    /* MP4 demuxer + codecs */
    ns_mp4_demuxer *demux;
    bool tracks_ready;
    bool has_video;
    bool has_audio;
    guint32 v_track_id;
    guint32 v_timescale;
    gint64  v_edit;
    guint32 a_track_id;
    guint32 a_timescale;
    gint64  a_edit;

    ipo_h264_dec_t *h264;
    ns_aac_decoder *aac;

    /* Dynamic pending video sample ring */
    mp4_vq_item_t *vq;
    int vq_cap;
    int vq_head;
    int vq_tail;
    int vq_count;

    /* Seek range extraction buffer */
    bool seek_mode;
    uint8_t *seek_buf;
    size_t seek_len;
    size_t seek_cap;
    uint64_t seek_file_pos;
    bool seeking_skip_audio;

    /* Decoded XRGB32 video frame buffer */
    uint32_t *rgb_buf;
    int rgb_w;
    int rgb_h;
    int src_w;
    int src_h;
    bool has_frame;
    bool dirty;
    uint32_t decoded_frames;
    uint64_t net_bytes;
    bool stream_eof;
    bool demux_eof;

    /* SB16 audio ring buffer */
    int audio_rate;
    int audio_channels;
    int16_t *audio_ring;
    size_t audio_ring_frames;
    size_t audio_expected_frames;
    size_t audio_frames_written;
    audio_stream_t *audio_stream;
    float *aac_tmp;
    size_t aac_tmp_floats;

    /* Playback state & clock */
    bool paused;
    bool audio_buf_paused;
    bool muted;
    bool playback_started;
    bool end_reached;
    uint32_t pos_ms;
    uint32_t duration_ms;
    uint32_t audio_base_ms;
    uint32_t clock_last_ms;
};

static void mp4_set_str(char **dst, const char *s) {
    char *copy = s ? strdup(s) : NULL;
    free(*dst);
    *dst = copy;
}

static void mp4_set_fmt(char **dst, const char *fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0) {
        va_end(ap);
        return;
    }
    char *buf = (char *)malloc((size_t)need + 1u);
    if (buf) {
        vsnprintf(buf, (size_t)need + 1u, fmt, ap);
        free(*dst);
        *dst = buf;
    }
    va_end(ap);
}

static char *mp4_fmt_alloc(const char *fmt, ...) {
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
    if (buf) vsnprintf(buf, (size_t)need + 1u, fmt, ap);
    va_end(ap);
    return buf;
}

static bool vq_push(mp4_player_t *p, ns_mp4_sample smp, uint32_t pts_ms) {
    if (!p) return false;
    if (p->vq_count >= p->vq_cap) {
        int new_cap = p->vq_cap ? p->vq_cap * 2 : 64;
        if (new_cap <= p->vq_cap || (size_t)new_cap > SIZE_MAX / sizeof(mp4_vq_item_t)) return false;
        mp4_vq_item_t *nvq = (mp4_vq_item_t *)malloc((size_t)new_cap * sizeof(mp4_vq_item_t));
        if (!nvq) return false;
        for (int i = 0; i < p->vq_count; i++) {
            nvq[i] = p->vq[(p->vq_head + i) % p->vq_cap];
        }
        free(p->vq);
        p->vq = nvq;
        p->vq_cap = new_cap;
        p->vq_head = 0;
        p->vq_tail = p->vq_count;
    }
    p->vq[p->vq_tail].smp = smp;
    p->vq[p->vq_tail].pts_ms = pts_ms;
    p->vq_tail = (p->vq_tail + 1) % p->vq_cap;
    p->vq_count++;
    return true;
}

static mp4_vq_item_t vq_pop(mp4_player_t *p) {
    mp4_vq_item_t it = p->vq[p->vq_head];
    p->vq[p->vq_head].smp.data = NULL;
    p->vq_head = (p->vq_head + 1) % p->vq_cap;
    p->vq_count--;
    return it;
}

static uint32_t mp4_millis(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
}

static uint32_t track_time_to_ms(gint64 pts, gint64 edit, guint32 timescale) {
    if (timescale == 0) return 0;
    gint64 adj = pts - (edit > 0 ? edit : 0);
    if (adj <= 0) return 0;
    return (uint32_t)(((uint64_t)adj * 1000ULL) / (uint64_t)timescale);
}

static void clear_vq(mp4_player_t *p) {
    while (p->vq_count > 0 && p->vq && p->vq_cap > 0) {
        if (p->vq[p->vq_head].smp.data) {
            g_bytes_unref(p->vq[p->vq_head].smp.data);
        }
        p->vq[p->vq_head].smp.data = NULL;
        p->vq_head = (p->vq_head + 1) % p->vq_cap;
        p->vq_count--;
    }
    p->vq_head = 0;
    p->vq_tail = 0;
}

static void destroy_audio_stream(mp4_player_t *p) {
    if (p->audio_stream) {
        audio_stream_stop(p->audio_stream);
        audio_stream_destroy(p->audio_stream);
        p->audio_stream = NULL;
    }
}

static void recreate_audio_stream(mp4_player_t *p) {
    destroy_audio_stream(p);
    if (!p->has_audio || !p->audio_ring || p->audio_ring_frames == 0 || p->audio_rate <= 0) return;

    double total_sec = (p->duration_ms > 0) ? ((double)p->duration_ms / 1000.0) : 600.0;
    double rem_sec = total_sec - ((double)p->audio_base_ms / 1000.0);
    if (rem_sec < 10.0) rem_sec = 10.0;
    p->audio_expected_frames = (size_t)((rem_sec + 4.0) * (double)p->audio_rate);
    p->audio_frames_written = 0;
    memset(p->audio_ring, 0, p->audio_ring_frames * 2u * sizeof(int16_t));

    audio_init();
    p->audio_stream = audio_stream_create_ring(AUDIO_FORMAT_S16, 2, (uint32_t)p->audio_rate,
                                               p->audio_ring, p->audio_ring_frames,
                                               p->audio_expected_frames);
    if (p->audio_stream) {
        audio_stream_set_volume(p->audio_stream, p->muted ? 0.0f : 0.90f);
        audio_stream_update_buffered(p->audio_stream, 0, false);
    }
}

static size_t est_played_frames(const mp4_player_t *p) {
    if (!p->audio_stream) return 0;
    ipo_audio_clock_t clk;
    memset(&clk, 0, sizeof(clk));
    clk.struct_size = (uint32_t)sizeof(clk);
    if (ipo_audio_get_stream_clock(p->audio_stream, &clk) == 0 && clk.played_frames > 0u) {
        size_t played = (size_t)clk.played_frames;
        if (played > p->audio_frames_written) played = p->audio_frames_written;
        return played;
    }
    if (p->audio_expected_frames == 0) return 0;
    double prog = audio_stream_get_progress(p->audio_stream);
    if (prog <= 0.0) return 0;
    if (prog >= 1.0) return p->audio_frames_written;
    size_t est = (size_t)(prog * (double)p->audio_expected_frames);
    if (est > p->audio_frames_written) est = p->audio_frames_written;
    return est;
}

static void reset_media_state(mp4_player_t *p) {
    yt_http_stream_close(&p->http);
    destroy_audio_stream(p);
    clear_vq(p);

    if (p->aac) { ns_aac_decoder_free(p->aac); p->aac = NULL; }
    if (p->h264) { ipo_h264_destroy(p->h264); p->h264 = NULL; }
    if (p->demux) { ns_mp4_demuxer_free(p->demux); p->demux = NULL; }

    p->tracks_ready = false;
    p->has_video = false;
    p->has_audio = false;
    p->seek_mode = false;
    p->seek_len = 0;
    p->seek_file_pos = 0;
    p->seeking_skip_audio = false;
    p->has_frame = false;
    p->decoded_frames = 0;
    p->net_bytes = 0;
    p->stream_eof = false;
    p->demux_eof = false;
    p->playback_started = false;
    p->end_reached = false;
    p->pos_ms = 0;
    p->duration_ms = 0;
    p->audio_base_ms = 0;
    p->audio_frames_written = 0;
    p->audio_expected_frames = 0;
    p->yt_json_len = 0;
}

mp4_player_t *mp4_player_create(void) {
    mp4_player_t *p = (mp4_player_t *)calloc(1, sizeof(mp4_player_t));
    if (!p) return NULL;
    p->http.sock = -1;
    p->aac_tmp_floats = 8192u;
    p->aac_tmp = (float *)malloc(p->aac_tmp_floats * sizeof(float));
    mp4_set_str(&p->status_text, "Idle");
    return p;
}

void mp4_player_destroy(mp4_player_t *p) {
    if (!p) return;
    reset_media_state(p);
    free(p->vq);
    free(p->input_url);
    free(p->video_id);
    free(p->stream_url);
    free(p->stream_ua);
    free(p->title);
    free(p->author);
    free(p->status_text);
    if (p->yt_json) free(p->yt_json);
    if (p->seek_buf) free(p->seek_buf);
    if (p->rgb_buf) free(p->rgb_buf);
    if (p->audio_ring) free(p->audio_ring);
    if (p->aac_tmp) free(p->aac_tmp);
    free(p);
}

void mp4_player_stop(mp4_player_t *p) {
    if (!p) return;
    reset_media_state(p);
    p->state = MP4_ST_IDLE;
    p->dirty = true;
}

static bool is_direct_mp4_url(const char *u) {
    if (!u) return false;
    const char *end = u;
    while (*end && *end != '?' && *end != '#') end++;
    if (end - u >= 4 &&
        (strncasecmp(end - 4, ".mp4", 4) == 0 || strncasecmp(end - 4, ".m4v", 4) == 0)) {
        return true;
    }
    return false;
}

int mp4_player_open(mp4_player_t *p, const char *url) {
    if (!p) return -1;
    reset_media_state(p);

    const char *clean = (url && *url) ? url : "https://www.youtube.com/watch?v=YE7VzlLtp-4";
    if (strncmp(clean, "ipo-video:", 10) == 0) clean += 10;
    mp4_set_str(&p->input_url, clean);
    p->paused = false;
    p->dirty = true;
    p->clock_last_ms = mp4_millis();

    if (is_direct_mp4_url(clean)) {
        mp4_set_str(&p->stream_url, clean);
        mp4_set_str(&p->stream_ua, "Mozilla/5.0 (IPO_OS; Links/2.22)");
        mp4_set_str(&p->title, "MP4 Stream");
        mp4_set_str(&p->status_text, "Connecting MP4 stream...");
        p->demux = ns_mp4_demuxer_new();
        if (!p->demux || !p->stream_url ||
            yt_http_stream_open(&p->http, "GET", p->stream_url, NULL, NULL, 0,
                                p->stream_ua, false, 0) != 0) {
            mp4_set_fmt(&p->status_text, "Error: %s",
                        (p->http.err_msg && p->http.err_msg[0]) ? p->http.err_msg : "stream open failed");
            p->state = MP4_ST_ERROR;
            return -1;
        }
        p->state = MP4_ST_STREAM;
        return 0;
    }

    free(p->video_id);
    p->video_id = yt_extract_video_id_alloc(clean);
    if (!p->video_id) {
        p->video_id = strdup("YE7VzlLtp-4");
    }
    mp4_set_fmt(&p->title, "YouTube [%s]", p->video_id ? p->video_id : "");
    mp4_set_fmt(&p->status_text, "Resolving YouTube stream [%s]...", p->video_id ? p->video_id : "");

    if (!p->yt_json) {
        p->yt_json_cap = 262144u;
        p->yt_json = (char *)malloc(p->yt_json_cap);
        if (!p->yt_json) {
            p->state = MP4_ST_ERROR;
            mp4_set_str(&p->status_text, "OOM resolving video");
            return -1;
        }
    }
    p->yt_json_len = 0;

    char *payload = mp4_fmt_alloc(
        "{\"videoId\":\"%s\",\"contentCheckOk\":true,\"racyCheckOk\":true,"
        "\"context\":{\"client\":{\"clientName\":\"ANDROID\",\"clientVersion\":\"20.10.38\","
        "\"osName\":\"Android\",\"osVersion\":\"14\",\"androidSdkVersion\":34,"
        "\"hl\":\"en\",\"gl\":\"US\"}}}",
        p->video_id ? p->video_id : "");
    if (!payload) {
        mp4_set_str(&p->status_text, "OOM resolving video");
        p->state = MP4_ST_ERROR;
        return -1;
    }

    int rc = yt_http_stream_open(&p->http, "POST",
                                 "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
                                 "application/json", payload, strlen(payload),
                                 YT_ANDROID_UA, false, 0);
    free(payload);
    if (rc != 0) {
        mp4_set_fmt(&p->status_text, "Innertube error: %s",
                    p->http.err_msg ? p->http.err_msg : "error");
        p->state = MP4_ST_ERROR;
        return -1;
    }
    p->state = MP4_ST_YT_RESOLVE;
    return 0;
}

static void setup_tracks_from_demux(mp4_player_t *p) {
    if (p->tracks_ready || !p->demux || !ns_mp4_demuxer_has_init(p->demux)) return;
    p->tracks_ready = true;

    guint n = ns_mp4_demuxer_n_tracks(p->demux);
    for (guint i = 0; i < n; i++) {
        const ns_mp4_track *tr = ns_mp4_demuxer_track(p->demux, i);
        if (!tr) continue;
        if (!p->has_video && strcmp(tr->handler, "vide") == 0 &&
            (strcmp(tr->codec, "avc1") == 0 || strcmp(tr->codec, "avc3") == 0)) {
            p->h264 = ipo_h264_create();
            if (p->h264) {
                gsize cfg_len = 0;
                const uint8_t *cfg = tr->codec_config ? (const uint8_t *)g_bytes_get_data(tr->codec_config, &cfg_len) : NULL;
                if (!cfg || cfg_len == 0 || ipo_h264_set_avcc(p->h264, cfg, cfg_len) == 0) {
                    p->has_video = true;
                    p->v_track_id = tr->id;
                    p->v_timescale = tr->timescale ? tr->timescale : 90000u;
                    p->v_edit = tr->edit_media_time;
                    p->src_w = tr->width;
                    p->src_h = tr->height;
                } else {
                    ipo_h264_destroy(p->h264);
                    p->h264 = NULL;
                }
            }
        } else if (!p->has_audio && strcmp(tr->handler, "soun") == 0 &&
                   strcmp(tr->codec, "mp4a") == 0 && tr->codec_config) {
            gsize asc_len = 0;
            const uint8_t *asc = (const uint8_t *)g_bytes_get_data(tr->codec_config, &asc_len);
            if (asc && asc_len > 0) {
                p->aac = ns_aac_decoder_new(asc, asc_len);
                if (p->aac) {
                    p->has_audio = true;
                    p->a_track_id = tr->id;
                    p->a_timescale = tr->timescale ? tr->timescale : 44100u;
                    p->a_edit = tr->edit_media_time;
                    p->audio_rate = ns_aac_decoder_sample_rate(p->aac);
                    p->audio_channels = ns_aac_decoder_channels(p->aac);
                    if (p->audio_rate <= 0) p->audio_rate = tr->sample_rate > 0 ? tr->sample_rate : 44100;
                    if (p->audio_channels <= 0) p->audio_channels = tr->channels > 0 ? tr->channels : 2;
                }
            }
        }
    }

    double idx_dur = ns_mp4_demuxer_index_duration(p->demux);
    if (idx_dur > 0.5) {
        p->duration_ms = (uint32_t)(idx_dur * 1000.0);
    }

    if (p->has_audio) {
        size_t need_frames = (size_t)p->audio_rate * 8u; /* 8 seconds stereo ring */
        if (need_frames > p->audio_ring_frames) {
            int16_t *nr = (int16_t *)realloc(p->audio_ring, need_frames * 2u * sizeof(int16_t));
            if (nr) {
                p->audio_ring = nr;
                p->audio_ring_frames = need_frames;
            }
        }
        recreate_audio_stream(p);
    }

    serial_printf("[yt_mp4] init tracks: video=%d (%dx%d ts=%u) audio=%d (%dHz %dch) dur=%ums\n",
                  p->has_video ? 1 : 0, p->src_w, p->src_h, (unsigned)p->v_timescale,
                  p->has_audio ? 1 : 0, p->audio_rate, p->audio_channels, (unsigned)p->duration_ms);
    if (p->has_video) {
        mp4_set_fmt(&p->status_text, "Decoding H.264 %dx%d + AAC...", p->src_w, p->src_h);
    } else {
        mp4_set_str(&p->status_text, "Error: No H.264 track found");
        p->state = MP4_ST_ERROR;
    }
    p->dirty = true;
}

static size_t audio_ahead_frames(const mp4_player_t *p) {
    if (!p->has_audio) return 0;
    size_t played = est_played_frames(p);
    return (p->audio_frames_written > played) ? (p->audio_frames_written - played) : 0;
}

static void append_aac_pcm_to_ring(mp4_player_t *p, const float *src, int frames, int ch) {
    if (!p->audio_ring || p->audio_ring_frames == 0 || frames <= 0) return;
    for (int f = 0; f < frames; f++) {
        float fl = (ch >= 1) ? src[f * ch + 0] : 0.0f;
        float fr = (ch >= 2) ? src[f * ch + 1] : fl;
        if (fl > 1.0f) fl = 1.0f; else if (fl < -1.0f) fl = -1.0f;
        if (fr > 1.0f) fr = 1.0f; else if (fr < -1.0f) fr = -1.0f;
        size_t idx = (p->audio_frames_written % p->audio_ring_frames) * 2u;
        p->audio_ring[idx + 0] = (int16_t)(fl * 32767.0f);
        p->audio_ring[idx + 1] = (int16_t)(fr * 32767.0f);
        p->audio_frames_written++;
    }
    if (p->audio_stream) {
        audio_stream_update_buffered(p->audio_stream, p->audio_frames_written, p->demux_eof);
    }
}

static void pump_samples_from_demux(mp4_player_t *p) {
    if (!p->tracks_ready) return;
    bool steady = p->playback_started && !p->audio_buf_paused &&
                  p->vq_count >= 16 &&
                  (!p->has_audio || audio_ahead_frames(p) >= (size_t)p->audio_rate);
    int max_iters = steady ? 16 : 96;
    for (int iter = 0; iter < max_iters; iter++) {
        if (p->has_audio && audio_ahead_frames(p) + 4096u >= p->audio_ring_frames) break;

        ns_mp4_sample smp;
        if (!ns_mp4_demuxer_pop_sample(p->demux, &smp)) break;

        if (smp.track_id == p->v_track_id) {
            uint32_t pts_ms = track_time_to_ms(smp.pts, p->v_edit, p->v_timescale);
            if (!vq_push(p, smp, pts_ms)) {
                if (smp.data) g_bytes_unref(smp.data);
                break;
            }
        } else if (p->has_audio && smp.track_id == p->a_track_id && p->aac) {
            uint32_t a_ms = track_time_to_ms(smp.pts, p->a_edit, p->a_timescale);
            if (p->seeking_skip_audio && a_ms + 30u < p->audio_base_ms) {
                if (smp.data) g_bytes_unref(smp.data);
                continue;
            }
            if (p->seeking_skip_audio) {
                p->audio_base_ms = a_ms;
                p->seeking_skip_audio = false;
            }
            gsize slen = 0;
            const guint8 *sdata = smp.data ? (const guint8 *)g_bytes_get_data(smp.data, &slen) : NULL;
            if (sdata && slen > 0 && p->aac_tmp) {
                int max_f = (int)(p->aac_tmp_floats / (size_t)(p->audio_channels > 0 ? p->audio_channels : 2));
                int nf = ns_aac_decode_frame(p->aac, sdata, slen, p->aac_tmp, max_f);
                if (nf > 0) {
                    append_aac_pcm_to_ring(p, p->aac_tmp, nf, p->audio_channels);
                }
            }
            if (smp.data) g_bytes_unref(smp.data);
        } else {
            if (smp.data) g_bytes_unref(smp.data);
        }
    }
    if (p->stream_eof && p->audio_stream) {
        audio_stream_update_buffered(p->audio_stream, p->audio_frames_written, true);
    }
}

static void feed_seek_bytes(mp4_player_t *p, const uint8_t *buf, size_t n) {
    if (!p || !buf || n == 0) return;
    if (p->seek_len + n > p->seek_cap) {
        size_t nc = p->seek_cap ? p->seek_cap * 2u : 131072u;
        while (nc < p->seek_len + n) nc *= 2u;
        uint8_t *nb = (uint8_t *)realloc(p->seek_buf, nc);
        if (!nb) return;
        p->seek_buf = nb;
        p->seek_cap = nc;
    }
    memcpy(p->seek_buf + p->seek_len, buf, n);
    p->seek_len += n;

    guint64 next_off = ns_mp4_demuxer_index_extract(p->demux, p->seek_file_pos,
                                                     p->seek_buf, p->seek_len);
    if (next_off == NS_MP4_INDEX_END) {
        p->seek_len = 0;
        p->stream_eof = true;
        p->demux_eof = true;
    } else if (next_off > p->seek_file_pos) {
        guint64 adv = next_off - p->seek_file_pos;
        if (adv <= p->seek_len) {
            memmove(p->seek_buf, p->seek_buf + adv, p->seek_len - (size_t)adv);
            p->seek_len -= (size_t)adv;
            p->seek_file_pos = next_off;
        } else {
            /* Keep seek_file_pos aligned with the next unread byte from the HTTP stream */
            p->seek_file_pos += p->seek_len;
            p->seek_len = 0;
        }
    }
}

static void pump_network_into_demux(mp4_player_t *p) {
    if (p->stream_eof) return;
    bool need_moov = !p->tracks_ready;
    if (!need_moov) {
        if (p->vq_count >= 75 && (!p->has_audio || audio_ahead_frames(p) >= (size_t)(p->audio_rate * 3))) {
            return;
        }
    }

    uint8_t buf[16384];
    bool steady_net = !need_moov && p->playback_started && !p->audio_buf_paused &&
                      p->vq_count >= 20 &&
                      (!p->has_audio || audio_ahead_frames(p) >= (size_t)(p->audio_rate * 3 / 2));
    int max_loops = need_moov ? 16 : (steady_net ? 2 : ((p->vq_count < 16 || (p->has_audio && audio_ahead_frames(p) < (size_t)p->audio_rate)) ? 10 : 4));
    for (int i = 0; i < max_loops && !p->stream_eof; i++) {
        bool eof = false;
        int n = yt_http_stream_read(&p->http, buf, sizeof(buf), &eof);
        if (n < 0) {
            mp4_set_fmt(&p->status_text, "Stream error: %s",
                        p->http.err_msg ? p->http.err_msg : "read error");
            p->state = MP4_ST_ERROR;
            return;
        }
        if (n > 0) {
            uint64_t chunk_start = p->net_bytes;
            p->net_bytes += (uint64_t)n;
            if (!p->seek_mode) {
                if (!ns_mp4_demuxer_append(p->demux, buf, (gsize)n)) {
                    mp4_set_str(&p->status_text, "MP4 parse error");
                    p->state = MP4_ST_ERROR;
                    return;
                }
                if (!p->tracks_ready && ns_mp4_demuxer_has_init(p->demux)) {
                    setup_tracks_from_demux(p);
                    if (p->state != MP4_ST_ERROR && ns_mp4_demuxer_has_sample_index(p->demux)) {
                        guint64 first_off = ns_mp4_demuxer_index_seek(p->demux, 0.0);
                        if (first_off == NS_MP4_INDEX_END) {
                            p->stream_eof = true;
                            p->demux_eof = true;
                        } else if (first_off < chunk_start) {
                            /* moov resided after mdat: issue a Range seek back to sample #0 */
                            mp4_player_seek_ms(p, 0);
                            return;
                        } else {
                            p->seek_mode = true;
                            p->seek_file_pos = chunk_start;
                            p->seek_len = 0;
                            feed_seek_bytes(p, buf, (size_t)n);
                        }
                    }
                }
            } else {
                feed_seek_bytes(p, buf, (size_t)n);
            }
            if (!p->tracks_ready && (p->net_bytes & 0xFFFFu) < (uint64_t)n) {
                mp4_set_fmt(&p->status_text, "Buffering MP4 (%lu KB)...",
                            (unsigned long)(p->net_bytes / 1024u));
                p->dirty = true;
            }
        }
        if (eof) {
            p->stream_eof = true;
            p->demux_eof = true;
            yt_http_stream_close(&p->http);
            break;
        }
        if (n == 0) break;
    }
}

static bool ensure_rgb_buf(mp4_player_t *p, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (p->rgb_buf && p->rgb_w == w && p->rgb_h == h) return true;
    uint32_t *nb = (uint32_t *)realloc(p->rgb_buf, (size_t)w * (size_t)h * sizeof(uint32_t));
    if (!nb) return false;
    p->rgb_buf = nb;
    p->rgb_w = w;
    p->rgb_h = h;
    return true;
}

static void decode_one_video_item(mp4_player_t *p, const mp4_vq_item_t *it,
                                  int target_w, int target_h, bool convert_rgb) {
    gsize slen = 0;
    const uint8_t *sdata = it->smp.data ? (const uint8_t *)g_bytes_get_data(it->smp.data, &slen) : NULL;
    if (!sdata || slen == 0 || !p->h264) return;

    uint32_t *dst = NULL;
    if (convert_rgb && ensure_rgb_buf(p, target_w, target_h)) {
        dst = p->rgb_buf;
    }
    uint64_t out_ts = 0;
    int sw = 0, sh = 0;
    int rc = ipo_h264_decode_xrgb32(p->h264, sdata, slen, it->pts_ms, &out_ts,
                                    dst, dst ? p->rgb_w : 0, dst ? p->rgb_h : 0, &sw, &sh);
    if (rc > 0) {
        if (sw > 0 && sh > 0) { p->src_w = sw; p->src_h = sh; }
        p->decoded_frames++;
        if (dst) {
            p->has_frame = true;
            p->dirty = true;
        }
        if ((p->decoded_frames == 1u) || (p->decoded_frames % 45u) == 0u) {
            serial_printf("[yt_mp4] decoded=%u pos=%ums/%ums src=%dx%d vq=%d audio_written=%lu\n",
                          (unsigned)p->decoded_frames, (unsigned)p->pos_ms, (unsigned)p->duration_ms,
                          p->src_w, p->src_h, p->vq_count, (unsigned long)p->audio_frames_written);
        }
    }
}

void mp4_player_tick(mp4_player_t *p, uint32_t now_ms, int target_w, int target_h) {
    if (!p || p->state == MP4_ST_IDLE || p->state == MP4_ST_ERROR) return;

    if (p->state == MP4_ST_YT_RESOLVE) {
        uint8_t tmp[8192];
        for (int iter = 0; iter < 8; iter++) {
            bool eof = false;
            int n = yt_http_stream_read(&p->http, tmp, sizeof(tmp), &eof);
            if (n < 0) {
                mp4_set_fmt(&p->status_text, "YT resolve error: %s",
                            p->http.err_msg ? p->http.err_msg : "error");
                p->state = MP4_ST_ERROR;
                yt_http_stream_close(&p->http);
                p->dirty = true;
                return;
            }
            if (n > 0) {
                if (p->yt_json_len + (size_t)n + 1 > p->yt_json_cap) {
                    size_t nc = p->yt_json_cap * 2u;
                    while (nc < p->yt_json_len + (size_t)n + 1) nc *= 2u;
                    char *nb = (char *)realloc(p->yt_json, nc);
                    if (!nb) break;
                    p->yt_json = nb;
                    p->yt_json_cap = nc;
                }
                memcpy(p->yt_json + p->yt_json_len, tmp, (size_t)n);
                p->yt_json_len += (size_t)n;
            }
            if (eof) {
                p->yt_json[p->yt_json_len] = '\0';
                yt_http_stream_close(&p->http);

                yt_player_info_t info;
                if (yt_parse_player_json(p->yt_json, p->yt_json_len, p->video_id, &info) != 0) {
                    mp4_set_str(&p->status_text,
                                (info.error_msg && info.error_msg[0]) ? info.error_msg : "YouTube playback unavailable");
                    yt_player_info_free(&info);
                    p->state = MP4_ST_ERROR;
                    p->dirty = true;
                    return;
                }
                if (info.title && info.title[0]) mp4_set_str(&p->title, info.title);
                if (info.author && info.author[0]) mp4_set_str(&p->author, info.author);
                p->duration_ms = info.duration_ms;
                mp4_set_str(&p->stream_url, info.stream_url ? info.stream_url : "");
                mp4_set_str(&p->stream_ua, YT_ANDROID_UA);
                mp4_set_fmt(&p->status_text, "Connecting MP4 (%dx%d)...",
                            info.width, info.height);
                serial_printf("[yt_mp4] resolved id=%s title=\"%s\" %dx%d@%dfps dur=%ums\n",
                              p->video_id ? p->video_id : "",
                              p->title ? p->title : "",
                              info.width, info.height, info.fps,
                              (unsigned)p->duration_ms);
                yt_player_info_free(&info);

                p->demux = ns_mp4_demuxer_new();
                if (!p->demux || !p->stream_url ||
                    yt_http_stream_open(&p->http, "GET", p->stream_url, NULL, NULL, 0,
                                        p->stream_ua, false, 0) != 0) {
                    mp4_set_fmt(&p->status_text, "Stream open error: %s",
                                p->http.err_msg ? p->http.err_msg : "error");
                    p->state = MP4_ST_ERROR;
                    p->dirty = true;
                    return;
                }
                p->state = MP4_ST_STREAM;
                p->dirty = true;
                return;
            }
            if (n == 0) break;
        }
        return;
    }

    /* MP4_ST_STREAM */
    pump_network_into_demux(p);
    if (p->state == MP4_ST_ERROR) { p->dirty = true; return; }
    pump_samples_from_demux(p);
    if (!p->tracks_ready || !p->has_video) return;

    /* Initial pre-roll decode so frame #1 displays immediately */
    if (!p->has_frame && p->vq_count > 0) {
        while (p->vq_count > 0 && !p->has_frame) {
            mp4_vq_item_t it = vq_pop(p);
            bool draw_now = (it.pts_ms + 40u >= p->pos_ms) || (p->vq_count == 0);
            decode_one_video_item(p, &it, target_w, target_h, draw_now);
            if (it.smp.data) g_bytes_unref(it.smp.data);
        }
    }

    if (!p->playback_started) {
        bool audio_ok = !p->has_audio || (p->audio_frames_written >= (size_t)(p->audio_rate / 2)) || p->stream_eof;
        bool video_ok = p->has_frame && (p->vq_count >= 10 || p->stream_eof);
        if (video_ok && audio_ok) {
            p->playback_started = true;
            p->clock_last_ms = now_ms;
            if (!p->paused && p->audio_stream) audio_stream_play(p->audio_stream);
            mp4_set_str(&p->status_text, "Playing");
            p->dirty = true;
        } else {
            p->clock_last_ms = now_ms;
            return;
        }
    }

    if (p->paused) {
        p->clock_last_ms = now_ms;
        return;
    }

    uint32_t dt = now_ms - p->clock_last_ms;
    p->clock_last_ms = now_ms;
    if (dt > 100u) dt = 100u;

    /* Pause advancing clock if starving mid-stream while more network bytes remain */
    if (p->vq_count == 0 && !p->stream_eof) {
        if (!p->audio_buf_paused) {
            if (p->audio_stream) audio_stream_pause(p->audio_stream);
            p->audio_buf_paused = true;
            mp4_set_str(&p->status_text, "Buffering...");
            p->dirty = true;
        }
        return;
    } else if (p->audio_buf_paused) {
        bool rebuf_ok = p->stream_eof ||
                        (p->vq_count >= 8 && (!p->has_audio || audio_ahead_frames(p) >= (size_t)(p->audio_rate / 3)));
        if (!rebuf_ok) {
            return;
        }
        p->audio_buf_paused = false;
        mp4_set_str(&p->status_text, "Playing");
        if (p->audio_stream && !p->paused) {
            audio_stream_play(p->audio_stream);
        }
    }

    p->pos_ms += dt;
    if (p->has_audio && p->audio_stream && p->audio_frames_written > 0) {
        size_t played = est_played_frames(p);
        if (played > 0) {
            uint32_t a_clk = p->audio_base_ms +
                (uint32_t)(((uint64_t)played * 1000ULL) / (uint64_t)p->audio_rate);
            int32_t drift = (int32_t)p->pos_ms - (int32_t)a_clk;
            if (drift > 45 || drift < -45) {
                p->pos_ms = a_clk;
            } else {
                p->pos_ms = (uint32_t)((int32_t)p->pos_ms - (drift / 4));
            }
        }
    }
    if (p->duration_ms > 0 && p->pos_ms > p->duration_ms) {
        p->pos_ms = p->duration_ms;
    }

    /* Count ready frames up to current clock, pacing 1 frame per tick unless >90ms behind */
    int ready_n = 0;
    for (int i = 0; i < p->vq_count; i++) {
        int idx = (p->vq_head + i) % p->vq_cap;
        if (p->vq[idx].pts_ms <= p->pos_ms) ready_n++;
        else break;
    }
    int dec_n = 0;
    if (ready_n > 0) {
        uint32_t head_pts = p->vq[p->vq_head].pts_ms;
        uint32_t lag_ms = (p->pos_ms > head_pts) ? (p->pos_ms - head_pts) : 0u;
        if (lag_ms < 90u) {
            dec_n = 1;
        } else if (lag_ms < 180u) {
            dec_n = (ready_n > 2) ? 2 : ready_n;
        } else {
            dec_n = (ready_n > 3) ? 3 : ready_n;
        }
    }
    for (int i = 0; i < dec_n; i++) {
        mp4_vq_item_t it = vq_pop(p);
        decode_one_video_item(p, &it, target_w, target_h, (i == dec_n - 1));
        if (it.smp.data) g_bytes_unref(it.smp.data);
    }

    if (p->stream_eof && p->vq_count == 0 && !p->end_reached) {
        uint32_t out_ts = 0;
        int sw = 0, sh = 0;
        if (p->h264 && ensure_rgb_buf(p, target_w, target_h)) {
            uint64_t ts64 = 0;
            if (ipo_h264_drain_xrgb32(p->h264, &ts64, p->rgb_buf, p->rgb_w, p->rgb_h, &sw, &sh) > 0) {
                (void)out_ts;
                p->decoded_frames++;
                p->has_frame = true;
                p->dirty = true;
            } else {
                p->end_reached = true;
                p->dirty = true;
            }
        } else {
            p->end_reached = true;
            p->dirty = true;
        }
    }
    if (dec_n > 0) {
        p->dirty = true;
    } else {
        static uint32_t s_last_bar_ms = 0;
        if (now_ms - s_last_bar_ms >= 50u) {
            s_last_bar_ms = now_ms;
            p->dirty = true;
        }
    }
}

void mp4_player_set_paused(mp4_player_t *p, bool paused) {
    if (!p) return;
    p->paused = paused;
    p->clock_last_ms = mp4_millis();
    if (p->audio_stream) {
        if (paused) audio_stream_pause(p->audio_stream);
        else if (p->playback_started) audio_stream_play(p->audio_stream);
    }
    p->dirty = true;
}

void mp4_player_set_muted(mp4_player_t *p, bool muted) {
    if (!p) return;
    p->muted = muted;
    if (p->audio_stream) {
        audio_stream_set_volume(p->audio_stream, muted ? 0.0f : 0.90f);
    }
    p->dirty = true;
}

void mp4_player_seek_ms(mp4_player_t *p, uint32_t pos_ms) {
    if (!p || !p->demux || !ns_mp4_demuxer_has_sample_index(p->demux)) return;
    if (p->duration_ms > 1000u && pos_ms >= p->duration_ms) pos_ms = p->duration_ms - 1000u;

    guint64 offset = ns_mp4_demuxer_index_seek(p->demux, (double)pos_ms / 1000.0);
    if (offset == NS_MP4_INDEX_END) {
        offset = 0;
        pos_ms = 0;
    }

    clear_vq(p);
    ns_mp4_demuxer_reset_parser(p->demux);
    if (p->h264) ipo_h264_flush(p->h264);
    if (p->aac) ns_aac_decoder_reset(p->aac);
    yt_http_stream_close(&p->http);

    p->seek_mode = true;
    p->seek_file_pos = offset;
    p->seek_len = 0;
    p->seeking_skip_audio = true;
    p->stream_eof = false;
    p->demux_eof = false;
    p->end_reached = false;
    p->playback_started = false;
    p->has_frame = false;
    p->pos_ms = pos_ms;
    p->audio_base_ms = pos_ms;
    p->clock_last_ms = mp4_millis();
    recreate_audio_stream(p);

    if (yt_http_stream_open(&p->http, "GET", p->stream_url, NULL, NULL, 0,
                            p->stream_ua, true, offset) != 0) {
        mp4_set_fmt(&p->status_text, "Seek failed: %s",
                    p->http.err_msg ? p->http.err_msg : "error");
        p->state = MP4_ST_ERROR;
    } else {
        mp4_set_fmt(&p->status_text, "Seeking to %02u:%02u...",
                    (unsigned)(pos_ms / 60000u), (unsigned)((pos_ms / 1000u) % 60u));
        p->state = MP4_ST_STREAM;
    }
    p->dirty = true;
}

bool mp4_player_is_paused(const mp4_player_t *p) { return p ? p->paused : false; }
bool mp4_player_is_muted(const mp4_player_t *p) { return p ? p->muted : false; }
uint32_t mp4_player_pos_ms(const mp4_player_t *p) { return p ? p->pos_ms : 0u; }
uint32_t mp4_player_duration_ms(const mp4_player_t *p) { return (p && p->duration_ms > 0) ? p->duration_ms : 60000u; }
const char *mp4_player_title(const mp4_player_t *p) { return (p && p->title && p->title[0]) ? p->title : "YouTube Stream"; }
const char *mp4_player_status_text(const mp4_player_t *p) { return (p && p->status_text && p->status_text[0]) ? p->status_text : "Idle"; }
bool mp4_player_has_frame(const mp4_player_t *p) { return p ? p->has_frame : false; }
bool mp4_player_has_audio(const mp4_player_t *p) { return p ? p->has_audio : false; }
int  mp4_player_audio_rate(const mp4_player_t *p) { return p ? p->audio_rate : 0; }

const uint32_t *mp4_player_frame_pixels(const mp4_player_t *p, int *out_w, int *out_h,
                                        int *out_src_w, int *out_src_h, uint32_t *out_decoded_cnt) {
    if (!p || !p->has_frame || !p->rgb_buf) return NULL;
    if (out_w) *out_w = p->rgb_w;
    if (out_h) *out_h = p->rgb_h;
    if (out_src_w) *out_src_w = p->src_w;
    if (out_src_h) *out_src_h = p->src_h;
    if (out_decoded_cnt) *out_decoded_cnt = p->decoded_frames;
    return p->rgb_buf;
}

bool mp4_player_take_dirty(mp4_player_t *p) {
    if (!p) return false;
    bool d = p->dirty;
    p->dirty = false;
    return d;
}
