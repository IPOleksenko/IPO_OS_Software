#include "youtube_client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syscall.h>
#include <unistd.h>

static char *yt_fmt_alloc(const char *fmt, ...) {
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

static void yt_set_str(char **dst, const char *s) {
    char *copy = s ? strdup(s) : NULL;
    free(*dst);
    *dst = copy;
}

static void yt_set_err(yt_http_stream_t *st, const char *fmt, ...) {
    if (!st) return;
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
        free(st->err_msg);
        st->err_msg = buf;
    }
    va_end(ap);
}

static inline uint32_t yt_millis(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
}

static int ci_cmp_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)tolower((unsigned char)a[i]);
        unsigned char cb = (unsigned char)tolower((unsigned char)b[i]);
        if (ca != cb) return (int)ca - (int)cb;
    }
    return 0;
}

static const char *ci_strstr(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return hay;
    size_t nlen = strlen(needle);
    for (; *hay; hay++) {
        if (ci_cmp_n(hay, needle, nlen) == 0) return hay;
    }
    return NULL;
}

static int parse_http_url_alloc(const char *url, char **out_host,
                                char **out_path, uint16_t *port, bool *is_https) {
    *out_host = NULL;
    *out_path = NULL;
    if (!url || !*url) return -1;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        *is_https = true;
        *port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        *is_https = false;
        *port = 80;
        p += 7;
    } else {
        return -1;
    }

    const char *sl = strchr(p, '/');
    const char *q = strchr(p, '?');
    if (q && (!sl || q < sl)) sl = q;
    size_t hostlen = sl ? (size_t)(sl - p) : strlen(p);
    if (hostlen == 0) return -1;

    const char *colon = NULL;
    for (size_t i = 0; i < hostlen; i++) {
        if (p[i] == ':') { colon = p + i; break; }
    }
    size_t hl = colon ? (size_t)(colon - p) : hostlen;
    if (hl == 0) return -1;
    char *host = (char *)malloc(hl + 1u);
    if (!host) return -1;
    memcpy(host, p, hl);
    host[hl] = '\0';
    if (colon) {
        int pv = atoi(colon + 1);
        if (pv > 0 && pv <= 65535) *port = (uint16_t)pv;
    }

    char *path = NULL;
    if (sl && *sl == '/') {
        path = strdup(sl);
    } else if (sl && *sl == '?') {
        path = yt_fmt_alloc("/%s", sl);
    } else {
        path = strdup("/");
    }
    if (!path) {
        free(host);
        return -1;
    }
    /* Trim URL fragment if present */
    char *hash = strchr(path, '#');
    if (hash) *hash = '\0';
    *out_host = host;
    *out_path = path;
    return 0;
}

static bool is_vid_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

char *yt_extract_video_id_alloc(const char *url) {
    if (!url) return NULL;

    const char *cand = NULL;
    const char *p = url;
    while ((p = strstr(p, "v=")) != NULL) {
        if (p == url || p[-1] == '?' || p[-1] == '&') {
            cand = p + 2;
            break;
        }
        p += 2;
    }
    if (!cand) {
        const char *m = strstr(url, "youtu.be/");
        if (m) cand = m + 9;
    }
    if (!cand) {
        const char *m = strstr(url, "/embed/");
        if (m) cand = m + 7;
    }
    if (!cand) {
        const char *m = strstr(url, "/shorts/");
        if (m) cand = m + 8;
    }
    if (!cand) {
        const char *m = strstr(url, "/v/");
        if (m) cand = m + 3;
    }
    if (!cand) return NULL;

    size_t len = 0;
    while (is_vid_char((unsigned char)cand[len])) len++;
    if (len < 11u) return NULL;
    if (len > 11u && cand[11] != '?' && cand[11] != '&' && cand[11] != '/' && cand[11] != '#') {
        return NULL;
    }
    char *out = (char *)malloc(12u);
    if (!out) return NULL;
    memcpy(out, cand, 11u);
    out[11] = '\0';
    return out;
}

static bool ends_with_ext_ci(const char *url, const char *ext) {
    if (!url || !ext) return false;
    const char *end = url;
    while (*end && *end != '?' && *end != '#') end++;
    size_t elen = strlen(ext);
    if ((size_t)(end - url) < elen) return false;
    return ci_cmp_n(end - elen, ext, elen) == 0;
}

yt_url_kind_t yt_classify_url(const char *url) {
    if (!url || !*url) return YT_URL_NONE;
    if (strncmp(url, "ipo-video:", 10) == 0) return YT_URL_VIDEO;
    if (strncmp(url, "links://video", 13) == 0) return YT_URL_VIDEO;
    if (strncmp(url, "yt://", 5) == 0) {
        if (strncmp(url + 5, "watch", 5) == 0 || strncmp(url + 5, "video", 5) == 0)
            return YT_URL_VIDEO;
        return YT_URL_BROWSE;
    }
    if (ends_with_ext_ci(url, ".mp4") || ends_with_ext_ci(url, ".m4v")) {
        return YT_URL_VIDEO;
    }

    char *host = NULL;
    char *path = NULL;
    uint16_t port = 0;
    bool is_https = false;
    if (parse_http_url_alloc(url, &host, &path, &port, &is_https) != 0) {
        return YT_URL_NONE;
    }
    yt_url_kind_t kind = YT_URL_NONE;
    if (ci_cmp_n(host, "youtu.be", 9) == 0) {
        kind = YT_URL_VIDEO;
    } else if (ci_cmp_n(host, "youtube.com", 12) == 0 ||
               ci_cmp_n(host, "www.youtube.com", 16) == 0 ||
               ci_cmp_n(host, "m.youtube.com", 14) == 0) {
        if (strncmp(path, "/watch", 6) == 0 ||
            strncmp(path, "/embed/", 7) == 0 ||
            strncmp(path, "/v/", 3) == 0 ||
            strncmp(path, "/shorts/", 8) == 0) {
            kind = YT_URL_VIDEO;
        } else if (strncmp(path, "/youtubei/", 10) != 0 && strncmp(path, "/api/", 5) != 0 &&
                   strncmp(path, "/s/", 3) != 0) {
            kind = YT_URL_BROWSE;
        }
    }
    free(host);
    free(path);
    return kind;
}

char *yt_url_decode_alloc(const char *src, size_t len) {
    if (!src) return strdup("");
    char *dst = (char *)malloc(len + 1u);
    if (!dst) return NULL;
    size_t di = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && i + 2 < len &&
                   isxdigit((unsigned char)src[i + 1]) &&
                   isxdigit((unsigned char)src[i + 2])) {
            char hx[3] = { src[i + 1], src[i + 2], '\0' };
            dst[di++] = (char)strtol(hx, NULL, 16);
            i += 2;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
    return dst;
}

/* =========================================================================
 * Reusable Non-Blocking HTTP/HTTPS Stream Client (BearSSL + Chunked/Range)
 * ========================================================================= */

static void close_socket_only(yt_http_stream_t *st) {
    if (st->ssl) {
        SSL_free(st->ssl);
        st->ssl = NULL;
    }
    if (st->ssl_ctx) {
        SSL_CTX_free(st->ssl_ctx);
        st->ssl_ctx = NULL;
    }
    if (st->sock >= 0) {
        close(st->sock);
        st->sock = -1;
    }
    if (st->req_buf) {
        free(st->req_buf);
        st->req_buf = NULL;
    }
    st->req_len = 0;
    st->req_sent = 0;
}

void yt_http_stream_close(yt_http_stream_t *st) {
    if (!st) return;
    close_socket_only(st);
    free(st->url); st->url = NULL;
    free(st->host); st->host = NULL;
    free(st->path); st->path = NULL;
    free(st->method); st->method = NULL;
    free(st->content_type); st->content_type = NULL;
    free(st->user_agent); st->user_agent = NULL;
    free(st->hdr_buf); st->hdr_buf = NULL;
    st->hdr_len = 0;
    st->hdr_cap = 0;
    free(st->err_msg); st->err_msg = NULL;
    if (st->redirect_history) {
        for (size_t i = 0; i < st->redirect_count; i++) free(st->redirect_history[i]);
        free(st->redirect_history);
        st->redirect_history = NULL;
    }
    st->redirect_count = 0;
    st->redirect_cap = 0;
    if (st->post_copy) {
        free(st->post_copy);
        st->post_copy = NULL;
    }
    st->post_len = 0;
    st->state = YT_HTTP_IDLE;
}

static bool hdr_ensure_cap(yt_http_stream_t *st, size_t need) {
    if (need <= st->hdr_cap) return true;
    size_t nc = st->hdr_cap ? st->hdr_cap : 8192u;
    while (nc < need) {
        if (nc > SIZE_MAX / 2u) { nc = need; break; }
        nc *= 2u;
    }
    char *nb = (char *)realloc(st->hdr_buf, nc);
    if (!nb) return false;
    st->hdr_buf = nb;
    st->hdr_cap = nc;
    return true;
}

static int prepare_request_buf(yt_http_stream_t *st) {
    const char *ua = (st->user_agent && st->user_agent[0]) ? st->user_agent : "Mozilla/5.0";
    const char *ct = (st->content_type && st->content_type[0]) ? st->content_type : "application/json";
    char *hdr = NULL;
    if (st->method && strcmp(st->method, "POST") == 0) {
        hdr = yt_fmt_alloc(
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %lu\r\n"
            "Origin: https://www.youtube.com\r\n"
            "Referer: https://www.youtube.com/\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n\r\n",
            st->path ? st->path : "/", st->host ? st->host : "",
            ua, ct, (unsigned long)st->post_len);
    } else if (st->use_range) {
        hdr = yt_fmt_alloc(
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Range: bytes=%llu-\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n\r\n",
            st->path ? st->path : "/", st->host ? st->host : "",
            ua, (unsigned long long)st->range_start);
    } else {
        hdr = yt_fmt_alloc(
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n\r\n",
            st->path ? st->path : "/", st->host ? st->host : "", ua);
    }
    if (!hdr) return -1;
    size_t hdr_len = strlen(hdr);
    if (st->post_copy && st->post_len > 0) {
        if (hdr_len > SIZE_MAX - st->post_len) { free(hdr); return -1; }
        char *nb = (char *)realloc(hdr, hdr_len + st->post_len);
        if (!nb) { free(hdr); return -1; }
        hdr = nb;
        memcpy(hdr + hdr_len, st->post_copy, st->post_len);
        hdr_len += st->post_len;
    }
    st->req_buf = hdr;
    st->req_len = (int)hdr_len;
    st->req_sent = 0;
    return 0;
}

static int connect_current_url(yt_http_stream_t *st) {
    close_socket_only(st);
    free(st->host); st->host = NULL;
    free(st->path); st->path = NULL;
    if (parse_http_url_alloc(st->url, &st->host, &st->path, &st->port, &st->is_https) != 0) {
        yt_set_err(st, "Invalid URL");
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    struct hostent *he = gethostbyname(st->host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        yt_set_err(st, "DNS failed: %s", st->host);
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    st->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (st->sock < 0) {
        yt_set_err(st, "Socket alloc failed");
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(st->port);
    memcpy(&sin.sin_addr, he->h_addr_list[0], sizeof(sin.sin_addr));
    if (connect(st->sock, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        yt_set_err(st, "Connect failed: %s:%u", st->host, st->port);
        close_socket_only(st);
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    int fl = fcntl(st->sock, F_GETFL, 0);
    if (fl >= 0) fcntl(st->sock, F_SETFL, fl | O_NONBLOCK);

    st->hdr_len = 0;
    if (hdr_ensure_cap(st, 8192u)) st->hdr_buf[0] = '\0';
    st->status_code = 0;
    st->content_length = -1;
    st->total_size = -1;
    st->chunked = false;
    st->chunk_state = 0;
    st->chunk_rem = 0;
    st->last_rx_ms = yt_millis();

    if (prepare_request_buf(st) != 0) {
        yt_set_err(st, "OOM HTTP req");
        close_socket_only(st);
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    if (st->is_https) {
        st->ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!st->ssl_ctx) {
            yt_set_err(st, "SSL CTX failed");
            close_socket_only(st);
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        st->ssl = SSL_new(st->ssl_ctx);
        if (!st->ssl) {
            yt_set_err(st, "SSL new failed");
            close_socket_only(st);
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        SSL_set_fd(st->ssl, st->sock);
        SSL_set_tlsext_host_name(st->ssl, st->host);
        st->state = YT_HTTP_TLS_HS;
    } else {
        st->state = YT_HTTP_SEND;
    }
    return 0;
}

int yt_http_stream_open(yt_http_stream_t *st, const char *method, const char *url,
                        const char *content_type, const void *body, size_t body_len,
                        const char *user_agent, bool use_range, uint64_t range_start) {
    if (!st || !url) return -1;
    memset(st, 0, sizeof(*st));
    st->sock = -1;
    st->content_length = -1;
    st->total_size = -1;
    st->start_ms = yt_millis();
    yt_set_str(&st->url, url);
    yt_set_str(&st->method, (method && *method) ? method : "GET");
    if (content_type) yt_set_str(&st->content_type, content_type);
    if (user_agent) yt_set_str(&st->user_agent, user_agent);
    if (!st->url || !st->method) {
        yt_set_err(st, "OOM open");
        st->state = YT_HTTP_ERROR;
        return -1;
    }
    st->use_range = use_range;
    st->range_start = range_start;
    if (body && body_len > 0) {
        st->post_copy = (uint8_t *)malloc(body_len);
        if (!st->post_copy) {
            yt_set_err(st, "OOM POST body");
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        memcpy(st->post_copy, body, body_len);
        st->post_len = body_len;
    }
    return connect_current_url(st);
}

static size_t unchunk_bytes(yt_http_stream_t *st, const uint8_t *in, size_t in_len,
                            uint8_t *out, size_t out_cap, size_t *out_written) {
    size_t i = 0;
    size_t w = 0;
    while (i < in_len && st->chunk_state != 5 && w < out_cap) {
        if (st->chunk_state == 0) {
            uint8_t c = in[i++];
            if (c >= '0' && c <= '9') st->chunk_rem = (st->chunk_rem << 4) | (size_t)(c - '0');
            else if (c >= 'a' && c <= 'f') st->chunk_rem = (st->chunk_rem << 4) | (size_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') st->chunk_rem = (st->chunk_rem << 4) | (size_t)(c - 'A' + 10);
            else if (c == ';') st->chunk_state = 1;
            else if (c == '\n') st->chunk_state = (st->chunk_rem == 0) ? 5 : 2;
        } else if (st->chunk_state == 1) {
            if (in[i++] == '\n') st->chunk_state = (st->chunk_rem == 0) ? 5 : 2;
        } else if (st->chunk_state == 2) {
            size_t avail = in_len - i;
            size_t room = out_cap - w;
            size_t take = avail < st->chunk_rem ? avail : st->chunk_rem;
            if (take > room) take = room;
            memcpy(out + w, in + i, take);
            w += take;
            i += take;
            st->chunk_rem -= take;
            if (st->chunk_rem == 0) st->chunk_state = 3;
        } else if (st->chunk_state == 3) {
            uint8_t c = in[i++];
            if (c == '\n') st->chunk_state = 0;
            else if (c == '\r') st->chunk_state = 4;
        } else if (st->chunk_state == 4) {
            if (in[i++] == '\n') st->chunk_state = 0;
            else st->chunk_state = 0;
        }
    }
    *out_written = w;
    return i;
}

static void parse_response_headers(yt_http_stream_t *st) {
    const char *sp = strchr(st->hdr_buf, ' ');
    st->status_code = sp ? atoi(sp + 1) : 200;
    const char *te = ci_strstr(st->hdr_buf, "\nTransfer-Encoding:");
    if (te && ci_strstr(te, "chunked")) {
        const char *eol = strchr(te + 1, '\n');
        const char *ch = ci_strstr(te, "chunked");
        if (ch && (!eol || ch < eol)) st->chunked = true;
    }
    const char *cl = ci_strstr(st->hdr_buf, "\nContent-Length:");
    if (cl) {
        st->content_length = strtoll(cl + 16, NULL, 10);
        if (st->total_size < 0 && st->content_length >= 0) {
            st->total_size = (int64_t)st->range_start + st->content_length;
        }
    }
    const char *cr = ci_strstr(st->hdr_buf, "\nContent-Range:");
    if (cr) {
        const char *sl = strchr(cr, '/');
        const char *eol = strchr(cr + 1, '\n');
        if (sl && (!eol || sl < eol)) {
            int64_t tot = strtoll(sl + 1, NULL, 10);
            if (tot > 0) st->total_size = tot;
        }
    }
}

static int raw_read_once(yt_http_stream_t *st, uint8_t *buf, size_t cap, bool *would_block, bool *eof) {
    *would_block = false;
    *eof = false;
    ipo_syscall(IPO_SYSCALL_NET_POLL, 0u, NULL);
    if (st->is_https) {
        int r = SSL_read(st->ssl, buf, (int)cap);
        if (r > 0) return r;
        if (r == 0) { *eof = true; return 0; }
        int err = SSL_get_error(st->ssl, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            *would_block = true;
            return 0;
        }
        if (err == SSL_ERROR_ZERO_RETURN) {
            *eof = true;
            return 0;
        }
        return -1;
    } else {
        ssize_t r = recv(st->sock, buf, cap, 0);
        if (r > 0) return (int)r;
        if (r == 0) { *eof = true; return 0; }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *would_block = true;
            return 0;
        }
        return -1;
    }
}

int yt_http_stream_read(yt_http_stream_t *st, uint8_t *out, size_t cap, bool *out_eof) {
    if (out_eof) *out_eof = false;
    if (!st || st->state == YT_HTTP_IDLE || st->state == YT_HTTP_ERROR) {
        if (out_eof) *out_eof = true;
        return -1;
    }
    if (st->state == YT_HTTP_DONE) {
        if (out_eof) *out_eof = true;
        return 0;
    }

    uint32_t now = yt_millis();
    ipo_syscall(IPO_SYSCALL_NET_POLL, 0u, NULL);

    if (st->state == YT_HTTP_TLS_HS) {
        for (int hs_try = 0; hs_try < 12; hs_try++) {
            int hs = SSL_connect(st->ssl);
            if (hs == 1) {
                st->state = YT_HTTP_SEND;
                st->last_rx_ms = yt_millis();
                break;
            }
            int err = SSL_get_error(st->ssl, hs);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                yt_set_err(st, "TLS handshake failed");
                st->state = YT_HTTP_ERROR;
                return -1;
            }
            if (yt_millis() - st->last_rx_ms > 15000u) {
                yt_set_err(st, "TLS handshake timeout");
                st->state = YT_HTTP_ERROR;
                return -1;
            }
            if (hs_try < 11) {
                usleep(1000);
                ipo_syscall(IPO_SYSCALL_NET_POLL, 0u, NULL);
            } else {
                return 0;
            }
        }
    }

    if (st->state == YT_HTTP_SEND) {
        while (st->req_sent < st->req_len) {
            int w;
            if (st->is_https) {
                w = SSL_write(st->ssl, st->req_buf + st->req_sent, st->req_len - st->req_sent);
                if (w < 0) {
                    int err = SSL_get_error(st->ssl, w);
                    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return 0;
                    yt_set_err(st, "TLS write failed");
                    st->state = YT_HTTP_ERROR;
                    return -1;
                }
            } else {
                ssize_t sw = send(st->sock, st->req_buf + st->req_sent,
                                  (size_t)(st->req_len - st->req_sent), 0);
                if (sw < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                    yt_set_err(st, "Socket send failed");
                    st->state = YT_HTTP_ERROR;
                    return -1;
                }
                w = (int)sw;
            }
            if (w == 0) return 0;
            st->req_sent += w;
            st->last_rx_ms = now;
        }
        free(st->req_buf);
        st->req_buf = NULL;
        st->state = YT_HTTP_RECV_HDR;
    }

    if (st->state == YT_HTTP_RECV_HDR) {
        uint8_t tmp[4096];
        bool wb = false, eof = false;
        int r = raw_read_once(st, tmp, sizeof(tmp), &wb, &eof);
        if (r < 0) {
            yt_set_err(st, "Header read failed");
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        if (wb && r == 0) {
            if (now - st->last_rx_ms > 15000u) {
                yt_set_err(st, "Header timeout");
                st->state = YT_HTTP_ERROR;
                return -1;
            }
            return 0;
        }
        if (eof && r == 0) {
            yt_set_err(st, "Empty HTTP response");
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        st->last_rx_ms = now;
        if (!hdr_ensure_cap(st, st->hdr_len + (size_t)r + 1u)) {
            yt_set_err(st, "OOM HTTP headers");
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        memcpy(st->hdr_buf + st->hdr_len, tmp, (size_t)r);
        st->hdr_len += (size_t)r;
        st->hdr_buf[st->hdr_len] = '\0';

        char *sep = strstr(st->hdr_buf, "\r\n\r\n");
        size_t sep_len = 4;
        if (!sep) {
            sep = strstr(st->hdr_buf, "\n\n");
            sep_len = 2;
        }
        if (!sep) {
            return 0;
        }

        size_t hdr_bytes = (size_t)(sep - st->hdr_buf) + sep_len;
        size_t leftover = st->hdr_len - hdr_bytes;
        *sep = '\0';
        parse_response_headers(st);

        if (st->status_code == 301 || st->status_code == 302 || st->status_code == 303 ||
            st->status_code == 307 || st->status_code == 308) {
            const char *loc = ci_strstr(st->hdr_buf, "\nLocation:");
            if (loc) {
                loc += 10;
                while (*loc == ' ' || *loc == '\t') loc++;
                const char *eol = loc;
                while (*eol && *eol != '\r' && *eol != '\n') eol++;
                size_t llen = (size_t)(eol - loc);
                if (llen > 0) {
                    char *next_url = (char *)malloc(llen + 1u);
                    if (next_url) {
                        memcpy(next_url, loc, llen);
                        next_url[llen] = '\0';
                        bool cycle = false;
                        if (st->url && strcmp(st->url, next_url) == 0) cycle = true;
                        for (size_t hi = 0; hi < st->redirect_count && !cycle; hi++) {
                            if (strcmp(st->redirect_history[hi], next_url) == 0) cycle = true;
                        }
                        if (!cycle) {
                            if (st->redirect_count >= st->redirect_cap) {
                                size_t nc = st->redirect_cap ? st->redirect_cap * 2u : 8u;
                                char **nh = (char **)realloc(st->redirect_history, nc * sizeof(char *));
                                if (nh) {
                                    st->redirect_history = nh;
                                    st->redirect_cap = nc;
                                }
                            }
                            if (st->url && st->redirect_count < st->redirect_cap) {
                                st->redirect_history[st->redirect_count++] = st->url;
                                st->url = next_url;
                            } else {
                                yt_set_str(&st->url, next_url);
                                free(next_url);
                            }
                            if (st->status_code == 303) {
                                yt_set_str(&st->method, "GET");
                                if (st->post_copy) { free(st->post_copy); st->post_copy = NULL; }
                                st->post_len = 0;
                            }
                            return connect_current_url(st);
                        }
                        free(next_url);
                    }
                }
            }
        }

        if (st->status_code >= 400) {
            yt_set_err(st, "HTTP %d error", st->status_code);
            st->state = YT_HTTP_ERROR;
            return -1;
        }

        st->state = YT_HTTP_RECV_BODY;
        if (leftover > 0) {
            const uint8_t *rem_ptr = (const uint8_t *)(st->hdr_buf + hdr_bytes);
            if (!st->chunked) {
                size_t take = leftover < cap ? leftover : cap;
                memcpy(out, rem_ptr, take);
                if (take < leftover) {
                    /* Shift unconsumed leftover back into hdr_buf[0..] for next call */
                    memmove(st->hdr_buf, rem_ptr + take, leftover - take);
                    st->hdr_len = leftover - take;
                    st->status_code = -1; /* flag sentinel: buffered body tail in hdr_buf */
                } else {
                    st->hdr_len = 0;
                    st->status_code = -2;
                }
                return (int)take;
            } else {
                size_t written = 0;
                size_t used = unchunk_bytes(st, rem_ptr, leftover, out, cap, &written);
                if (used < leftover) {
                    memmove(st->hdr_buf, rem_ptr + used, leftover - used);
                    st->hdr_len = leftover - used;
                    st->status_code = -1;
                } else {
                    st->hdr_len = 0;
                    st->status_code = -2;
                }
                if (st->chunk_state == 5) {
                    st->state = YT_HTTP_DONE;
                    if (out_eof) *out_eof = true;
                }
                return (int)written;
            }
        }
        st->hdr_len = 0;
        st->status_code = -2;
    }

    if (st->state == YT_HTTP_RECV_BODY) {
        if (st->status_code == -1 && st->hdr_len > 0) {
            const uint8_t *rem_ptr = (const uint8_t *)st->hdr_buf;
            if (!st->chunked) {
                size_t take = st->hdr_len < cap ? st->hdr_len : cap;
                memcpy(out, rem_ptr, take);
                if (take < st->hdr_len) {
                    memmove(st->hdr_buf, rem_ptr + take, st->hdr_len - take);
                    st->hdr_len -= take;
                } else {
                    st->hdr_len = 0;
                    st->status_code = -2;
                }
                return (int)take;
            } else {
                size_t written = 0;
                size_t used = unchunk_bytes(st, rem_ptr, st->hdr_len, out, cap, &written);
                if (used < st->hdr_len) {
                    memmove(st->hdr_buf, rem_ptr + used, st->hdr_len - used);
                    st->hdr_len -= used;
                } else {
                    st->hdr_len = 0;
                    st->status_code = -2;
                }
                if (st->chunk_state == 5) {
                    st->state = YT_HTTP_DONE;
                    if (out_eof) *out_eof = true;
                }
                return (int)written;
            }
        }

        size_t req_cap = cap < 16384u ? cap : 16384u;
        uint8_t tmp[16384];
        bool wb = false, eof = false;
        int r = raw_read_once(st, st->chunked ? tmp : out, req_cap, &wb, &eof);
        if (r < 0) {
            st->state = YT_HTTP_DONE;
            if (out_eof) *out_eof = true;
            return 0;
        }
        if (r > 0) {
            st->last_rx_ms = now;
            if (!st->chunked) return r;
            size_t written = 0;
            size_t used = unchunk_bytes(st, tmp, (size_t)r, out, cap, &written);
            if (used < (size_t)r) {
                size_t rem = (size_t)r - used;
                if (hdr_ensure_cap(st, rem)) {
                    memcpy(st->hdr_buf, tmp + used, rem);
                    st->hdr_len = rem;
                    st->status_code = -1;
                }
            }
            if (st->chunk_state == 5) {
                st->state = YT_HTTP_DONE;
                if (out_eof) *out_eof = true;
            }
            return (int)written;
        }
        if (eof) {
            st->state = YT_HTTP_DONE;
            if (out_eof) *out_eof = true;
            return 0;
        }
        if (wb && (now - st->last_rx_ms > 20000u)) {
            yt_set_err(st, "Stream read timeout");
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        return 0;
    }
    return 0;
}

/* =========================================================================
 * Depth-Aware Lightweight JSON Scanner
 * ========================================================================= */

static size_t json_skip_string(const char *s, size_t end, size_t quote_pos) {
    size_t i = quote_pos + 1;
    while (i < end) {
        char c = s[i++];
        if (c == '\\' && i < end) {
            i++;
        } else if (c == '"') {
            return i;
        }
    }
    return end;
}

static size_t json_find_matching(const char *s, size_t end, size_t open_pos,
                                 char open_ch, char close_ch) {
    int depth = 0;
    for (size_t i = open_pos; i < end; i++) {
        char c = s[i];
        if (c == '"') {
            i = json_skip_string(s, end, i);
            if (i == 0) break;
            i--;
            continue;
        }
        if (c == open_ch) depth++;
        else if (c == close_ch) {
            depth--;
            if (depth == 0) return i + 1;
        }
    }
    return end;
}

static size_t json_skip_value(const char *s, size_t end, size_t pos) {
    while (pos < end && (unsigned char)s[pos] <= ' ') pos++;
    if (pos >= end) return end;
    char c = s[pos];
    if (c == '"') return json_skip_string(s, end, pos);
    if (c == '{') return json_find_matching(s, end, pos, '{', '}');
    if (c == '[') return json_find_matching(s, end, pos, '[', ']');
    while (pos < end && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' && (unsigned char)s[pos] > ' ') {
        pos++;
    }
    return pos;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void yt_sb_append_char(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 2u > *cap) {
        size_t nc = *cap ? (*cap * 2u) : 64u;
        while (nc < *len + 2u) {
            if (nc > SIZE_MAX / 2u) { nc = *len + 2u; break; }
            nc *= 2u;
        }
        char *nb = (char *)realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    (*buf)[(*len)++] = c;
    (*buf)[*len] = '\0';
}

static char *json_unescape_alloc(const char *s, size_t qstart, size_t qend) {
    char *dst = (char *)malloc(64u);
    size_t cap = dst ? 64u : 0u;
    size_t di = 0;
    if (!dst) return NULL;
    dst[0] = '\0';
    size_t i = (qstart < qend && s[qstart] == '"') ? qstart + 1 : qstart;
    size_t lim = (qend > i && s[qend - 1] == '"') ? qend - 1 : qend;
    while (i < lim) {
        char c = s[i++];
        if (c == '\\' && i < lim) {
            char e = s[i++];
            if (e == '"' || e == '\\' || e == '/') yt_sb_append_char(&dst, &di, &cap, e);
            else if (e == 'n' || e == 'r' || e == 't') yt_sb_append_char(&dst, &di, &cap, ' ');
            else if (e == 'u' && i + 4 <= lim) {
                int h0 = hex_val(s[i]), h1 = hex_val(s[i + 1]),
                    h2 = hex_val(s[i + 2]), h3 = hex_val(s[i + 3]);
                if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                    unsigned cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    i += 4;
                    if (cp < 0x80) {
                        yt_sb_append_char(&dst, &di, &cap, (char)cp);
                    } else if (cp < 0x800) {
                        yt_sb_append_char(&dst, &di, &cap, (char)(0xC0 | (cp >> 6)));
                        yt_sb_append_char(&dst, &di, &cap, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        yt_sb_append_char(&dst, &di, &cap, (char)(0xE0 | (cp >> 12)));
                        yt_sb_append_char(&dst, &di, &cap, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        yt_sb_append_char(&dst, &di, &cap, (char)(0x80 | (cp & 0x3F)));
                    }
                }
            }
        } else {
            yt_sb_append_char(&dst, &di, &cap, c);
        }
    }
    return dst;
}

/* Looks up a direct key inside a JSON object [obj_start, obj_end) where s[obj_start] == '{'. */
static bool json_obj_get(const char *s, size_t obj_start, size_t obj_end,
                         const char *key, size_t *val_start, size_t *val_end) {
    size_t klen = strlen(key);
    size_t i = obj_start + 1;
    while (i < obj_end) {
        while (i < obj_end && ((unsigned char)s[i] <= ' ' || s[i] == ',')) i++;
        if (i >= obj_end || s[i] == '}') break;
        if (s[i] != '"') { i++; continue; }
        size_t ks = i + 1;
        size_t ke = json_skip_string(s, obj_end, i);
        size_t key_raw_len = (ke > ks + 1) ? (ke - 1 - ks) : 0;
        i = ke;
        while (i < obj_end && (unsigned char)s[i] <= ' ') i++;
        if (i >= obj_end || s[i] != ':') continue;
        i++;
        while (i < obj_end && (unsigned char)s[i] <= ' ') i++;
        size_t vs = i;
        size_t ve = json_skip_value(s, obj_end, vs);
        if (key_raw_len == klen && memcmp(s + ks, key, klen) == 0) {
            *val_start = vs;
            *val_end = ve;
            return true;
        }
        i = ve;
    }
    return false;
}

static char *extract_yt_text_alloc(const char *s, size_t vs, size_t ve) {
    if (vs >= ve) return NULL;
    if (s[vs] == '"') {
        char *out = json_unescape_alloc(s, vs, ve);
        if (out && !out[0]) { free(out); return NULL; }
        return out;
    }
    if (s[vs] != '{') return NULL;
    size_t ss, se;
    if (json_obj_get(s, vs, ve, "simpleText", &ss, &se) && s[ss] == '"') {
        char *out = json_unescape_alloc(s, ss, se);
        if (out && !out[0]) { free(out); return NULL; }
        return out;
    }
    size_t rs, re;
    if (json_obj_get(s, vs, ve, "runs", &rs, &re) && rs < re && s[rs] == '[') {
        size_t p = rs + 1;
        while (p < re && (unsigned char)s[p] <= ' ') p++;
        if (p < re && s[p] == '{') {
            size_t item_end = json_find_matching(s, re, p, '{', '}');
            size_t ts, te;
            if (json_obj_get(s, p, item_end, "text", &ts, &te) && s[ts] == '"') {
                char *out = json_unescape_alloc(s, ts, te);
                if (out && !out[0]) { free(out); return NULL; }
                return out;
            }
        }
    }
    return NULL;
}

void yt_search_items_free(yt_search_item_t *items, int count) {
    if (!items) return;
    for (int i = 0; i < count; i++) {
        free(items[i].video_id);
        free(items[i].title);
        free(items[i].channel);
        free(items[i].duration);
        free(items[i].views);
        free(items[i].published);
    }
    free(items);
}

void yt_player_info_free(yt_player_info_t *info) {
    if (!info) return;
    free(info->video_id); info->video_id = NULL;
    free(info->title); info->title = NULL;
    free(info->author); info->author = NULL;
    free(info->stream_url); info->stream_url = NULL;
    free(info->error_msg); info->error_msg = NULL;
}

int yt_parse_search_json(const char *json, size_t len, yt_search_item_t **out_items) {
    if (out_items) *out_items = NULL;
    if (!json || !out_items) return 0;
    yt_search_item_t *items = NULL;
    int count = 0;
    int cap = 0;
    const char *needle = "\"videoRenderer\"";
    size_t nlen = strlen(needle);
    size_t pos = 0;
    while (pos + nlen < len) {
        const char *hit = NULL;
        for (size_t i = pos; i + nlen <= len; i++) {
            if (json[i] == '"' && memcmp(json + i, needle, nlen) == 0) {
                hit = json + i;
                break;
            }
        }
        if (!hit) break;
        size_t cur = (size_t)(hit - json) + nlen;
        while (cur < len && ((unsigned char)json[cur] <= ' ' || json[cur] == ':')) cur++;
        if (cur >= len || json[cur] != '{') { pos = cur + 1; continue; }
        size_t obj_end = json_find_matching(json, len, cur, '{', '}');

        yt_search_item_t it;
        memset(&it, 0, sizeof(it));
        size_t vs, ve;
        if (json_obj_get(json, cur, obj_end, "videoId", &vs, &ve) && json[vs] == '"') {
            it.video_id = json_unescape_alloc(json, vs, ve);
        }
        if (it.video_id && strlen(it.video_id) == 11) {
            if (json_obj_get(json, cur, obj_end, "title", &vs, &ve)) {
                it.title = extract_yt_text_alloc(json, vs, ve);
            }
            if (json_obj_get(json, cur, obj_end, "ownerText", &vs, &ve)) {
                it.channel = extract_yt_text_alloc(json, vs, ve);
            } else if (json_obj_get(json, cur, obj_end, "longBylineText", &vs, &ve)) {
                it.channel = extract_yt_text_alloc(json, vs, ve);
            }
            if (json_obj_get(json, cur, obj_end, "lengthText", &vs, &ve)) {
                it.duration = extract_yt_text_alloc(json, vs, ve);
            }
            if (json_obj_get(json, cur, obj_end, "shortViewCountText", &vs, &ve)) {
                it.views = extract_yt_text_alloc(json, vs, ve);
            } else if (json_obj_get(json, cur, obj_end, "viewCountText", &vs, &ve)) {
                it.views = extract_yt_text_alloc(json, vs, ve);
            }
            if (json_obj_get(json, cur, obj_end, "publishedTimeText", &vs, &ve)) {
                it.published = extract_yt_text_alloc(json, vs, ve);
            }
            bool dup = false;
            for (int k = 0; k < count; k++) {
                if (strcmp(items[k].video_id, it.video_id) == 0) { dup = true; break; }
            }
            if (!dup && it.title && it.title[0]) {
                if (count >= cap) {
                    int nc = cap ? cap * 2 : 16;
                    yt_search_item_t *ni = (yt_search_item_t *)realloc(items, (size_t)nc * sizeof(yt_search_item_t));
                    if (!ni) dup = true;
                    else { items = ni; cap = nc; }
                }
                if (!dup) {
                    items[count++] = it;
                    memset(&it, 0, sizeof(it));
                }
            }
        }
        free(it.video_id); free(it.title); free(it.channel);
        free(it.duration); free(it.views); free(it.published);
        pos = cur + 1;
    }
    *out_items = items;
    return count;
}

int yt_parse_player_json(const char *json, size_t len, const char *fallback_vid, yt_player_info_t *info) {
    if (!json || len == 0 || !info) return -1;
    memset(info, 0, sizeof(*info));
    if (fallback_vid) info->video_id = strdup(fallback_vid);

    size_t root = 0;
    while (root < len && (unsigned char)json[root] <= ' ') root++;
    if (root >= len || json[root] != '{') {
        info->error_msg = strdup("Malformed player JSON");
        return -1;
    }
    size_t root_end = json_find_matching(json, len, root, '{', '}');

    size_t ps_s, ps_e;
    if (json_obj_get(json, root, root_end, "playabilityStatus", &ps_s, &ps_e) && json[ps_s] == '{') {
        char *st = NULL;
        size_t vs, ve;
        if (json_obj_get(json, ps_s, ps_e, "status", &vs, &ve)) {
            st = json_unescape_alloc(json, vs, ve);
        }
        if (!st || strcmp(st, "OK") != 0) {
            if (json_obj_get(json, ps_s, ps_e, "reason", &vs, &ve)) {
                info->error_msg = json_unescape_alloc(json, vs, ve);
            } else {
                info->error_msg = yt_fmt_alloc("Playability: %s", (st && st[0]) ? st : "error");
            }
            free(st);
            return -1;
        }
        free(st);
    }

    size_t vd_s, vd_e;
    if (json_obj_get(json, root, root_end, "videoDetails", &vd_s, &vd_e) && json[vd_s] == '{') {
        size_t vs, ve;
        if (json_obj_get(json, vd_s, vd_e, "videoId", &vs, &ve)) {
            free(info->video_id);
            info->video_id = json_unescape_alloc(json, vs, ve);
        }
        if (json_obj_get(json, vd_s, vd_e, "title", &vs, &ve)) {
            free(info->title);
            info->title = json_unescape_alloc(json, vs, ve);
        }
        if (json_obj_get(json, vd_s, vd_e, "author", &vs, &ve)) {
            free(info->author);
            info->author = json_unescape_alloc(json, vs, ve);
        }
        if (json_obj_get(json, vd_s, vd_e, "lengthSeconds", &vs, &ve)) {
            char *lbuf = json_unescape_alloc(json, vs, ve);
            if (lbuf) {
                info->duration_ms = (uint32_t)(strtoul(lbuf, NULL, 10) * 1000u);
                free(lbuf);
            }
        }
    }

    size_t sd_s, sd_e;
    if (json_obj_get(json, root, root_end, "streamingData", &sd_s, &sd_e) && json[sd_s] == '{') {
        size_t fm_s, fm_e;
        if (json_obj_get(json, sd_s, sd_e, "formats", &fm_s, &fm_e) && json[fm_s] == '[') {
            char *best_url = NULL;
            int best_w = 0, best_h = 0, best_fps = 0;
            uint32_t best_dur = 0;

            size_t p = fm_s + 1;
            while (p < fm_e) {
                while (p < fm_e && ((unsigned char)json[p] <= ' ' || json[p] == ',')) p++;
                if (p >= fm_e || json[p] == ']') break;
                if (json[p] != '{') { p++; continue; }
                size_t ie = json_find_matching(json, fm_e, p, '{', '}');

                size_t vs, ve;
                int itag = 0, w = 0, h = 0, fps = 0;
                uint32_t adur = 0;
                char *url_buf = NULL;
                char *mime = NULL;
                if (json_obj_get(json, p, ie, "itag", &vs, &ve))
                    itag = (int)strtol(json + vs, NULL, 10);
                if (json_obj_get(json, p, ie, "width", &vs, &ve))
                    w = (int)strtol(json + vs, NULL, 10);
                if (json_obj_get(json, p, ie, "height", &vs, &ve))
                    h = (int)strtol(json + vs, NULL, 10);
                if (json_obj_get(json, p, ie, "fps", &vs, &ve))
                    fps = (int)strtol(json + vs, NULL, 10);
                if (json_obj_get(json, p, ie, "mimeType", &vs, &ve) && json[vs] == '"')
                    mime = json_unescape_alloc(json, vs, ve);
                if (json_obj_get(json, p, ie, "approxDurationMs", &vs, &ve)) {
                    char *dbuf = json_unescape_alloc(json, vs, ve);
                    if (dbuf) {
                        adur = (uint32_t)strtoul(dbuf, NULL, 10);
                        free(dbuf);
                    }
                }
                if (json_obj_get(json, p, ie, "url", &vs, &ve) && json[vs] == '"') {
                    url_buf = json_unescape_alloc(json, vs, ve);
                }

                if (url_buf && url_buf[0] && (itag == 18 || (mime && strstr(mime, "video/mp4") != NULL))) {
                    if (itag == 18 || !best_url) {
                        free(best_url);
                        best_url = url_buf;
                        url_buf = NULL;
                        best_w = w; best_h = h; best_fps = fps; best_dur = adur;
                    }
                }
                free(url_buf);
                free(mime);
                if (best_url && itag == 18) break;
                p = ie;
            }
            if (best_url) {
                info->stream_url = best_url;
                info->width = best_w;
                info->height = best_h;
                info->fps = best_fps;
                if (best_dur > 0) info->duration_ms = best_dur;
                return 0;
            }
        }
    }

    if (!info->error_msg || !info->error_msg[0]) {
        free(info->error_msg);
        info->error_msg = strdup("No progressive MP4 URL (itag 18) in stream");
    }
    return -1;
}

/* =========================================================================
 * Live YouTube Home / Search HTML Builder
 * ========================================================================= */

static char *extract_query_param_alloc(const char *url, const char *pname) {
    if (!url) return NULL;
    const char *q = strchr(url, '?');
    if (!q) return NULL;
    q++;
    size_t plen = strlen(pname);
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t part_len = amp ? (size_t)(amp - q) : strlen(q);
        const char *eq = memchr(q, '=', part_len);
        if (eq && (size_t)(eq - q) == plen && strncmp(q, pname, plen) == 0) {
            const char *val = eq + 1;
            size_t vlen = (size_t)((q + part_len) - val);
            return yt_url_decode_alloc(val, vlen);
        }
        if (!amp) break;
        q = amp + 1;
    }
    return NULL;
}

static char *json_escape_alloc(const char *src) {
    if (!src) return strdup("");
    size_t len = strlen(src);
    if (len > (SIZE_MAX - 1u) / 2u) return NULL;
    char *dst = (char *)malloc(len * 2u + 1u);
    if (!dst) return NULL;
    size_t di = 0;
    for (size_t i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[di++] = '\\';
            dst[di++] = (char)c;
        } else if (c < 0x20) {
            dst[di++] = ' ';
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
    return dst;
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} str_builder_t;

static void sb_append_n(str_builder_t *sb, const char *s, size_t n) {
    if (!s || n == 0) return;
    if (sb->len + n + 1 > sb->cap) {
        size_t nc = sb->cap ? sb->cap * 2 : 16384;
        while (nc < sb->len + n + 1) nc *= 2;
        char *nb = (char *)realloc(sb->buf, nc);
        if (!nb) return;
        sb->buf = nb;
        sb->cap = nc;
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}

static void sb_append(str_builder_t *sb, const char *s) {
    if (s) sb_append_n(sb, s, strlen(s));
}

static void sb_append_url_encoded(str_builder_t *sb, const char *src) {
    if (!src) return;
    for (size_t i = 0; src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            sb_append_n(sb, (const char *)&src[i], 1u);
        } else if (c == ' ') {
            sb_append_n(sb, "+", 1u);
        } else {
            char hx[4];
            snprintf(hx, sizeof(hx), "%%%02X", c);
            sb_append_n(sb, hx, 3u);
        }
    }
}

static void sb_append_html_esc(str_builder_t *sb, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
            case '&': sb_append(sb, "&amp;"); break;
            case '<': sb_append(sb, "&lt;"); break;
            case '>': sb_append(sb, "&gt;"); break;
            case '"': sb_append(sb, "&quot;"); break;
            default: sb_append_n(sb, s, 1); break;
        }
    }
}

int yt_build_browse_html(const char *url, char **out_html, size_t *out_len) {
    if (!out_html || !out_len) return -1;
    *out_html = NULL;
    *out_len = 0;

    char *query = extract_query_param_alloc(url, "search_query");
    if (!query || !query[0]) {
        free(query);
        query = extract_query_param_alloc(url, "q");
    }
    bool is_home = (!query || query[0] == '\0');
    if (is_home) {
        free(query);
        query = strdup("Blender Open Movie 4K");
    }

    char *q_esc = json_escape_alloc(query ? query : "");
    char *payload = yt_fmt_alloc(
        "{\"query\":\"%s\",\"context\":{\"client\":{\"clientName\":\"WEB\","
        "\"clientVersion\":\"2.20240726.00.00\",\"hl\":\"en\",\"gl\":\"US\"}}}",
        q_esc ? q_esc : "");
    free(q_esc);

    yt_http_stream_t st;
    memset(&st, 0, sizeof(st));
    st.sock = -1;
    int rc = payload ? yt_http_stream_open(&st, "POST",
        "https://www.youtube.com/youtubei/v1/search?prettyPrint=false",
        "application/json", payload, strlen(payload),
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36",
        false, 0) : -1;
    free(payload);

    size_t cap = 524288;
    char *resp = NULL;
    size_t rlen = 0;
    if (rc == 0) {
        resp = (char *)malloc(cap);
        if (resp) {
            uint8_t chunk[16384];
            uint32_t t0 = yt_millis();
            bool eof = false;
            while (!eof && yt_millis() - t0 < 12000u) {
                int n = yt_http_stream_read(&st, chunk, sizeof(chunk), &eof);
                if (n < 0) break;
                if (n > 0) {
                    if (rlen + (size_t)n + 1 > cap) {
                        if (cap > SIZE_MAX / 2u) break;
                        size_t nc = cap * 2u;
                        while (nc < rlen + (size_t)n + 1) {
                            if (nc > SIZE_MAX / 2u) { nc = rlen + (size_t)n + 1; break; }
                            nc *= 2u;
                        }
                        char *nb = (char *)realloc(resp, nc);
                        if (!nb) break;
                        resp = nb;
                        cap = nc;
                    }
                    memcpy(resp + rlen, chunk, (size_t)n);
                    rlen += (size_t)n;
                } else if (!eof) {
                    usleep(1000);
                }
            }
            resp[rlen] = '\0';
        }
    }
    char *err_copy = st.err_msg ? strdup(st.err_msg) : NULL;
    yt_http_stream_close(&st);

    yt_search_item_t *items = NULL;
    int nitems = resp ? yt_parse_search_json(resp, rlen, &items) : 0;
    if (resp) free(resp);

    str_builder_t sb = { NULL, 0, 0 };
    sb_append(&sb,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>");
    sb_append_html_esc(&sb, is_home ? "YouTube - IPO_OS" : (query ? query : ""));
    sb_append(&sb,
        " - YouTube</title>"
        "<style>"
        "body{background:#f8fafc;color:#0f172a;font-size:10px;margin:0;padding:4px;}"
        ".topbar{background:#ffffff;border:1px solid #e2e8f0;padding:6px;margin-bottom:6px;}"
        ".logo{background:#dc2626;color:#ffffff;font-weight:bold;padding:2px 6px;}"
        ".chips{margin:4px 0 6px 0;padding:2px 0;}"
        ".chip{background:#e2e8f0;color:#1e293b;padding:2px 6px;text-decoration:none;font-weight:bold;}"
        ".card{background:#ffffff;border:1px solid #cbd5e1;padding:5px;margin-bottom:5px;}"
        ".vtitle{color:#0f172a;font-weight:bold;font-size:11px;text-decoration:none;}"
        ".meta{color:#475569;font-size:9px;}"
        ".dur{background:#0f172a;color:#ffffff;font-weight:bold;padding:1px 4px;font-size:9px;}"
        ".playbtn{background:#dc2626;color:#ffffff;font-weight:bold;padding:2px 6px;text-decoration:none;}"
        "</style></head><body>"
        "<div class=\"topbar\">"
        "<a href=\"https://www.youtube.com/\" class=\"logo\">YouTube</a> "
        "<form action=\"https://www.youtube.com/results\" method=\"get\">"
        "<input type=\"text\" name=\"search_query\" value=\"");
    sb_append_html_esc(&sb, is_home ? "" : (query ? query : ""));
    sb_append(&sb, "\" size=\"22\"> <input type=\"submit\" value=\"Search\"></form>");

    static const char *chips[][2] = {
        { "Home", "https://www.youtube.com/" },
        { "Big Buck Bunny", "Big Buck Bunny" },
        { "Blender 4K", "Blender Open Movie" },
        { "Nature", "Nature Wildlife 4K" },
        { "Music", "Official Music Video" },
        { "Science", "Space Documentary 4K" }
    };
    sb_append(&sb, "<div class=\"chips\">");
    for (int c = 0; c < 6; c++) {
        if (c == 0) {
            sb_append(&sb, "<a class=\"chip\" href=\"https://www.youtube.com/\">Home</a> ");
        } else {
            sb_append(&sb, "<a class=\"chip\" href=\"https://www.youtube.com/results?search_query=");
            sb_append_url_encoded(&sb, chips[c][1]);
            sb_append(&sb, "\">");
            sb_append_html_esc(&sb, chips[c][0]);
            sb_append(&sb, "</a> ");
        }
    }
    sb_append(&sb, "</div></div>");

    if (nitems == 0) {
        serial_printf("[yt_browse] url=%s query=\"%s\" items=0 err=%s\n",
                      url ? url : "", query ? query : "", err_copy ? err_copy : "");
        sb_append(&sb, "<div class=\"card\"><b>No results loaded");
        if (err_copy && err_copy[0]) {
            sb_append(&sb, " (");
            sb_append_html_esc(&sb, err_copy);
            sb_append(&sb, ")");
        }
        sb_append(&sb, ".</b> Try direct demo: "
                  "<a href=\"https://www.youtube.com/watch?v=YE7VzlLtp-4\">Big Buck Bunny (Blender)</a>"
                  "</div>");
    } else {
        serial_printf("[yt_browse] url=%s query=\"%s\" items=%d\n",
                      url ? url : "", query ? query : "", nitems);
        sb_append(&sb, "<p class=\"meta\">Showing live YouTube results for: <b>");
        sb_append_html_esc(&sb, query ? query : "");
        sb_append(&sb, "</b></p>");
        for (int i = 0; i < nitems; i++) {
            const char *vid = items[i].video_id ? items[i].video_id : "";
            char *watch_url = yt_fmt_alloc("https://www.youtube.com/watch?v=%s", vid);
            char *thumb_url = yt_fmt_alloc("https://i.ytimg.com/vi/%s/mqdefault.jpg", vid);

            sb_append(&sb, "<div class=\"card\"><table border=\"0\" cellpadding=\"3\"><tr>"
                           "<td width=\"116\" valign=\"top\"><a href=\"");
            sb_append(&sb, watch_url ? watch_url : "");
            sb_append(&sb, "\"><img src=\"");
            sb_append(&sb, thumb_url ? thumb_url : "");
            sb_append(&sb, "\" width=\"112\" height=\"63\" alt=\"thumb\"></a></td><td valign=\"top\">"
                           "<a class=\"vtitle\" href=\"");
            sb_append(&sb, watch_url ? watch_url : "");
            sb_append(&sb, "\">");
            sb_append_html_esc(&sb, items[i].title ? items[i].title : "");
            sb_append(&sb, "</a><br><span class=\"meta\">");
            if (items[i].channel && items[i].channel[0]) {
                sb_append_html_esc(&sb, items[i].channel);
            }
            if (items[i].views && items[i].views[0]) {
                sb_append(&sb, " &bull; ");
                sb_append_html_esc(&sb, items[i].views);
            }
            if (items[i].duration && items[i].duration[0]) {
                sb_append(&sb, " &nbsp;<span class=\"dur\">");
                sb_append_html_esc(&sb, items[i].duration);
                sb_append(&sb, "</span>");
            }
            sb_append(&sb, "</span><br><br><a class=\"playbtn\" href=\"");
            sb_append(&sb, watch_url ? watch_url : "");
            sb_append(&sb, "\">Play Video</a></td></tr></table></div>");
            free(watch_url);
            free(thumb_url);
        }
    }
    yt_search_items_free(items, nitems);
    free(err_copy);
    free(query);

    sb_append(&sb, "</body></html>");
    *out_html = sb.buf;
    *out_len = sb.len;
    return sb.buf ? 0 : -1;
}
