#ifndef LINKS_YOUTUBE_CLIENT_H
#define LINKS_YOUTUBE_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <openssl/ssl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YT_URL_NONE = 0,
    YT_URL_BROWSE = 1,
    YT_URL_VIDEO = 2
} yt_url_kind_t;

typedef enum {
    YT_HTTP_IDLE = 0,
    YT_HTTP_TLS_HS,
    YT_HTTP_SEND,
    YT_HTTP_RECV_HDR,
    YT_HTTP_RECV_BODY,
    YT_HTTP_DONE,
    YT_HTTP_ERROR
} yt_http_state_t;

typedef struct yt_http_stream {
    yt_http_state_t state;
    char url[4096];
    char host[256];
    char path[3840];
    uint16_t port;
    bool is_https;
    int sock;
    SSL_CTX *ssl_ctx;
    SSL *ssl;

    char method[8];
    char content_type[64];
    char user_agent[160];
    uint8_t *post_copy;
    size_t post_len;
    bool use_range;
    uint64_t range_start;

    char *req_buf;
    int req_len;
    int req_sent;

    char hdr_buf[16384];
    size_t hdr_len;
    int status_code;
    int64_t content_length;
    int64_t total_size;
    bool chunked;
    int chunk_state;
    size_t chunk_rem;
    int redirects;
    uint32_t start_ms;
    uint32_t last_rx_ms;
    char err_msg[128];
} yt_http_stream_t;

typedef struct yt_search_item {
    char video_id[16];
    char title[160];
    char channel[80];
    char duration[24];
    char views[48];
    char published[48];
} yt_search_item_t;

typedef struct yt_player_info {
    char video_id[16];
    char title[160];
    char author[80];
    uint32_t duration_ms;
    int width;
    int height;
    int fps;
    char stream_url[4096];
    char error_msg[160];
} yt_player_info_t;

yt_url_kind_t yt_classify_url(const char *url);
int  yt_extract_video_id(const char *url, char *out_id, size_t out_cap);
void yt_url_decode(const char *src, size_t len, char *dst, size_t cap);

int  yt_http_stream_open(yt_http_stream_t *st, const char *method, const char *url,
                         const char *content_type, const void *body, size_t body_len,
                         const char *user_agent, bool use_range, uint64_t range_start);
void yt_http_stream_close(yt_http_stream_t *st);
/* Reads decoded HTTP body data into out (up to cap). Returns >0 bytes read, 0 if would-block or eof (*out_eof). */
int  yt_http_stream_read(yt_http_stream_t *st, uint8_t *out, size_t cap, bool *out_eof);

/* Parses ANDROID player Innertube JSON buffer into info. Returns 0 on playable OK, -1 otherwise. */
int  yt_parse_player_json(const char *json, size_t len, const char *fallback_vid, yt_player_info_t *info);

/* Parses WEB search Innertube JSON into items (up to max_items). Returns item count. */
int  yt_parse_search_json(const char *json, size_t len, yt_search_item_t *items, int max_items);

/* Synchronous helper for YouTube HTML page rendering (used by Links yt/https browse handler).
 * Caller must free(*out_html) with free(). Returns 0 on success. */
int  yt_build_browse_html(const char *url, char **out_html, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* LINKS_YOUTUBE_CLIENT_H */
