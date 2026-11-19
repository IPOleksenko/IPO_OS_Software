#include "youtube_client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syscall.h>
#include <unistd.h>

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

static int parse_http_url(const char *url, char *host, size_t hcap,
                          char *path, size_t pcap, uint16_t *port, bool *is_https) {
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
    if (colon) {
        size_t hl = (size_t)(colon - p);
        if (hl == 0 || hl >= hcap) return -1;
        memcpy(host, p, hl);
        host[hl] = '\0';
        int pv = atoi(colon + 1);
        if (pv > 0 && pv <= 65535) *port = (uint16_t)pv;
    } else {
        if (hostlen >= hcap) return -1;
        memcpy(host, p, hostlen);
        host[hostlen] = '\0';
    }

    if (sl && *sl == '/') {
        snprintf(path, pcap, "%s", sl);
    } else if (sl && *sl == '?') {
        snprintf(path, pcap, "/%s", sl);
    } else {
        snprintf(path, pcap, "/");
    }
    /* Trim URL fragment if present */
    char *hash = strchr(path, '#');
    if (hash) *hash = '\0';
    return 0;
}

static bool is_vid_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

int yt_extract_video_id(const char *url, char *out_id, size_t out_cap) {
    if (!url || !out_id || out_cap < 12) return -1;
    out_id[0] = '\0';

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
    if (!cand) return -1;

    size_t i = 0;
    while (i < 11 && is_vid_char((unsigned char)cand[i])) {
        out_id[i] = cand[i];
        i++;
    }
    if (i != 11) {
        out_id[0] = '\0';
        return -1;
    }
    out_id[11] = '\0';
    return 0;
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

    char host[256], path[3840];
    uint16_t port = 0;
    bool is_https = false;
    if (parse_http_url(url, host, sizeof(host), path, sizeof(path), &port, &is_https) != 0) {
        return YT_URL_NONE;
    }
    if (ci_cmp_n(host, "youtu.be", 9) == 0) {
        return YT_URL_VIDEO;
    }
    if (ci_cmp_n(host, "youtube.com", 12) == 0 ||
        ci_cmp_n(host, "www.youtube.com", 16) == 0 ||
        ci_cmp_n(host, "m.youtube.com", 14) == 0) {
        if (strncmp(path, "/watch", 6) == 0 ||
            strncmp(path, "/embed/", 7) == 0 ||
            strncmp(path, "/v/", 3) == 0 ||
            strncmp(path, "/shorts/", 8) == 0) {
            return YT_URL_VIDEO;
        }
        if (strncmp(path, "/youtubei/", 10) == 0 || strncmp(path, "/api/", 5) == 0 ||
            strncmp(path, "/s/", 3) == 0) {
            return YT_URL_NONE;
        }
        return YT_URL_BROWSE;
    }
    return YT_URL_NONE;
}

void yt_url_decode(const char *src, size_t len, char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    size_t di = 0;
    for (size_t i = 0; i < len && di + 1 < cap; i++) {
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
}

static void url_encode_append(const char *src, char *dst, size_t cap) {
    size_t di = strlen(dst);
    for (size_t i = 0; src[i] && di + 4 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else if (c == ' ') {
            dst[di++] = '+';
        } else {
            snprintf(dst + di, cap - di, "%%%02X", c);
            di += 3;
        }
    }
    dst[di] = '\0';
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
    if (st->post_copy) {
        free(st->post_copy);
        st->post_copy = NULL;
    }
    st->post_len = 0;
    st->state = YT_HTTP_IDLE;
}

static int prepare_request_buf(yt_http_stream_t *st) {
    size_t hdr_cap = 4608 + st->post_len;
    st->req_buf = (char *)malloc(hdr_cap);
    if (!st->req_buf) return -1;

    int off = 0;
    if (strcmp(st->method, "POST") == 0) {
        off = snprintf(st->req_buf, hdr_cap,
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
            st->path, st->host,
            st->user_agent[0] ? st->user_agent : "Mozilla/5.0",
            st->content_type[0] ? st->content_type : "application/json",
            (unsigned long)st->post_len);
    } else if (st->use_range) {
        off = snprintf(st->req_buf, hdr_cap,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Range: bytes=%llu-\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n\r\n",
            st->path, st->host,
            st->user_agent[0] ? st->user_agent : "Mozilla/5.0",
            (unsigned long long)st->range_start);
    } else {
        off = snprintf(st->req_buf, hdr_cap,
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: %s\r\n"
            "Accept: */*\r\n"
            "Accept-Encoding: identity\r\n"
            "Connection: close\r\n\r\n",
            st->path, st->host,
            st->user_agent[0] ? st->user_agent : "Mozilla/5.0");
    }
    if (off < 0 || (size_t)off >= hdr_cap) {
        free(st->req_buf);
        st->req_buf = NULL;
        return -1;
    }
    if (st->post_copy && st->post_len > 0) {
        if ((size_t)off + st->post_len > hdr_cap) {
            free(st->req_buf);
            st->req_buf = NULL;
            return -1;
        }
        memcpy(st->req_buf + off, st->post_copy, st->post_len);
        off += (int)st->post_len;
    }
    st->req_len = off;
    st->req_sent = 0;
    return 0;
}

static int connect_current_url(yt_http_stream_t *st) {
    close_socket_only(st);
    if (parse_http_url(st->url, st->host, sizeof(st->host),
                       st->path, sizeof(st->path), &st->port, &st->is_https) != 0) {
        snprintf(st->err_msg, sizeof(st->err_msg), "Invalid URL");
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    struct hostent *he = gethostbyname(st->host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        snprintf(st->err_msg, sizeof(st->err_msg), "DNS failed: %s", st->host);
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    st->sock = socket(AF_INET, SOCK_STREAM, 0);
    if (st->sock < 0) {
        snprintf(st->err_msg, sizeof(st->err_msg), "Socket alloc failed");
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(st->port);
    memcpy(&sin.sin_addr, he->h_addr_list[0], sizeof(sin.sin_addr));
    if (connect(st->sock, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        snprintf(st->err_msg, sizeof(st->err_msg), "Connect failed: %s:%u", st->host, st->port);
        close_socket_only(st);
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    int fl = fcntl(st->sock, F_GETFL, 0);
    if (fl >= 0) fcntl(st->sock, F_SETFL, fl | O_NONBLOCK);

    st->hdr_len = 0;
    st->hdr_buf[0] = '\0';
    st->status_code = 0;
    st->content_length = -1;
    st->total_size = -1;
    st->chunked = false;
    st->chunk_state = 0;
    st->chunk_rem = 0;
    st->last_rx_ms = yt_millis();

    if (prepare_request_buf(st) != 0) {
        snprintf(st->err_msg, sizeof(st->err_msg), "OOM HTTP req");
        close_socket_only(st);
        st->state = YT_HTTP_ERROR;
        return -1;
    }

    if (st->is_https) {
        st->ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!st->ssl_ctx) {
            snprintf(st->err_msg, sizeof(st->err_msg), "SSL CTX failed");
            close_socket_only(st);
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        st->ssl = SSL_new(st->ssl_ctx);
        if (!st->ssl) {
            snprintf(st->err_msg, sizeof(st->err_msg), "SSL new failed");
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
    snprintf(st->url, sizeof(st->url), "%s", url);
    snprintf(st->method, sizeof(st->method), "%s", (method && *method) ? method : "GET");
    if (content_type) snprintf(st->content_type, sizeof(st->content_type), "%s", content_type);
    if (user_agent) snprintf(st->user_agent, sizeof(st->user_agent), "%s", user_agent);
    st->use_range = use_range;
    st->range_start = range_start;
    if (body && body_len > 0) {
        st->post_copy = (uint8_t *)malloc(body_len);
        if (!st->post_copy) {
            snprintf(st->err_msg, sizeof(st->err_msg), "OOM POST body");
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
        int hs = SSL_connect(st->ssl);
        if (hs == 1) {
            st->state = YT_HTTP_SEND;
            st->last_rx_ms = now;
        } else {
            int err = SSL_get_error(st->ssl, hs);
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                snprintf(st->err_msg, sizeof(st->err_msg), "TLS handshake failed");
                st->state = YT_HTTP_ERROR;
                return -1;
            }
            if (now - st->last_rx_ms > 15000u) {
                snprintf(st->err_msg, sizeof(st->err_msg), "TLS handshake timeout");
                st->state = YT_HTTP_ERROR;
                return -1;
            }
            return 0;
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
                    snprintf(st->err_msg, sizeof(st->err_msg), "TLS write failed");
                    st->state = YT_HTTP_ERROR;
                    return -1;
                }
            } else {
                ssize_t sw = send(st->sock, st->req_buf + st->req_sent,
                                  (size_t)(st->req_len - st->req_sent), 0);
                if (sw < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                    snprintf(st->err_msg, sizeof(st->err_msg), "Socket send failed");
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
            snprintf(st->err_msg, sizeof(st->err_msg), "Header read failed");
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        if (wb && r == 0) {
            if (now - st->last_rx_ms > 15000u) {
                snprintf(st->err_msg, sizeof(st->err_msg), "Header timeout");
                st->state = YT_HTTP_ERROR;
                return -1;
            }
            return 0;
        }
        if (eof && r == 0) {
            snprintf(st->err_msg, sizeof(st->err_msg), "Empty HTTP response");
            st->state = YT_HTTP_ERROR;
            return -1;
        }
        st->last_rx_ms = now;
        size_t avail = sizeof(st->hdr_buf) - 1 - st->hdr_len;
        size_t copy = (size_t)r < avail ? (size_t)r : avail;
        memcpy(st->hdr_buf + st->hdr_len, tmp, copy);
        st->hdr_len += copy;
        st->hdr_buf[st->hdr_len] = '\0';

        char *sep = strstr(st->hdr_buf, "\r\n\r\n");
        size_t sep_len = 4;
        if (!sep) {
            sep = strstr(st->hdr_buf, "\n\n");
            sep_len = 2;
        }
        if (!sep) {
            if (st->hdr_len >= sizeof(st->hdr_buf) - 1) {
                snprintf(st->err_msg, sizeof(st->err_msg), "Headers too large");
                st->state = YT_HTTP_ERROR;
                return -1;
            }
            return 0;
        }

        size_t hdr_bytes = (size_t)(sep - st->hdr_buf) + sep_len;
        size_t leftover = st->hdr_len - hdr_bytes;
        *sep = '\0';
        parse_response_headers(st);

        if ((st->status_code == 301 || st->status_code == 302 || st->status_code == 303 ||
             st->status_code == 307 || st->status_code == 308) && st->redirects < 5) {
            const char *loc = ci_strstr(st->hdr_buf, "\nLocation:");
            if (loc) {
                loc += 10;
                while (*loc == ' ' || *loc == '\t') loc++;
                const char *eol = loc;
                while (*eol && *eol != '\r' && *eol != '\n') eol++;
                size_t llen = (size_t)(eol - loc);
                if (llen > 0 && llen < sizeof(st->url)) {
                    memcpy(st->url, loc, llen);
                    st->url[llen] = '\0';
                    st->redirects++;
                    if (st->status_code == 303) {
                        snprintf(st->method, sizeof(st->method), "GET");
                        if (st->post_copy) { free(st->post_copy); st->post_copy = NULL; }
                        st->post_len = 0;
                    }
                    return connect_current_url(st);
                }
            }
        }

        if (st->status_code >= 400) {
            snprintf(st->err_msg, sizeof(st->err_msg), "HTTP %d error", st->status_code);
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
                if (rem > sizeof(st->hdr_buf)) rem = sizeof(st->hdr_buf);
                memcpy(st->hdr_buf, tmp + used, rem);
                st->hdr_len = rem;
                st->status_code = -1;
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
            snprintf(st->err_msg, sizeof(st->err_msg), "Stream read timeout");
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

static void json_unescape_string(const char *s, size_t qstart, size_t qend, char *dst, size_t cap) {
    if (!dst || cap == 0) return;
    size_t di = 0;
    size_t i = (qstart < qend && s[qstart] == '"') ? qstart + 1 : qstart;
    size_t lim = (qend > i && s[qend - 1] == '"') ? qend - 1 : qend;
    while (i < lim && di + 4 < cap) {
        char c = s[i++];
        if (c == '\\' && i < lim) {
            char e = s[i++];
            if (e == '"' || e == '\\' || e == '/') dst[di++] = e;
            else if (e == 'n' || e == 'r' || e == 't') dst[di++] = ' ';
            else if (e == 'u' && i + 4 <= lim) {
                int h0 = hex_val(s[i]), h1 = hex_val(s[i + 1]),
                    h2 = hex_val(s[i + 2]), h3 = hex_val(s[i + 3]);
                if (h0 >= 0 && h1 >= 0 && h2 >= 0 && h3 >= 0) {
                    unsigned cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                    i += 4;
                    if (cp < 0x80) {
                        dst[di++] = (char)cp;
                    } else if (cp < 0x800) {
                        dst[di++] = (char)(0xC0 | (cp >> 6));
                        dst[di++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        dst[di++] = (char)(0xE0 | (cp >> 12));
                        dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        dst[di++] = (char)(0x80 | (cp & 0x3F));
                    }
                }
            }
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = '\0';
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

static bool extract_yt_text(const char *s, size_t vs, size_t ve, char *dst, size_t cap) {
    if (vs >= ve) return false;
    if (s[vs] == '"') {
        json_unescape_string(s, vs, ve, dst, cap);
        return dst[0] != '\0';
    }
    if (s[vs] != '{') return false;
    size_t ss, se;
    if (json_obj_get(s, vs, ve, "simpleText", &ss, &se) && s[ss] == '"') {
        json_unescape_string(s, ss, se, dst, cap);
        return dst[0] != '\0';
    }
    size_t rs, re;
    if (json_obj_get(s, vs, ve, "runs", &rs, &re) && rs < re && s[rs] == '[') {
        size_t p = rs + 1;
        while (p < re && (unsigned char)s[p] <= ' ') p++;
        if (p < re && s[p] == '{') {
            size_t item_end = json_find_matching(s, re, p, '{', '}');
            size_t ts, te;
            if (json_obj_get(s, p, item_end, "text", &ts, &te) && s[ts] == '"') {
                json_unescape_string(s, ts, te, dst, cap);
                return dst[0] != '\0';
            }
        }
    }
    return false;
}

int yt_parse_search_json(const char *json, size_t len, yt_search_item_t *items, int max_items) {
    if (!json || !items || max_items <= 0) return 0;
    int count = 0;
    const char *needle = "\"videoRenderer\"";
    size_t nlen = strlen(needle);
    size_t pos = 0;
    while (pos + nlen < len && count < max_items) {
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
            json_unescape_string(json, vs, ve, it.video_id, sizeof(it.video_id));
        }
        if (strlen(it.video_id) == 11) {
            if (json_obj_get(json, cur, obj_end, "title", &vs, &ve)) {
                extract_yt_text(json, vs, ve, it.title, sizeof(it.title));
            }
            if (json_obj_get(json, cur, obj_end, "ownerText", &vs, &ve)) {
                extract_yt_text(json, vs, ve, it.channel, sizeof(it.channel));
            } else if (json_obj_get(json, cur, obj_end, "longBylineText", &vs, &ve)) {
                extract_yt_text(json, vs, ve, it.channel, sizeof(it.channel));
            }
            if (json_obj_get(json, cur, obj_end, "lengthText", &vs, &ve)) {
                extract_yt_text(json, vs, ve, it.duration, sizeof(it.duration));
            }
            if (json_obj_get(json, cur, obj_end, "shortViewCountText", &vs, &ve)) {
                extract_yt_text(json, vs, ve, it.views, sizeof(it.views));
            } else if (json_obj_get(json, cur, obj_end, "viewCountText", &vs, &ve)) {
                extract_yt_text(json, vs, ve, it.views, sizeof(it.views));
            }
            if (json_obj_get(json, cur, obj_end, "publishedTimeText", &vs, &ve)) {
                extract_yt_text(json, vs, ve, it.published, sizeof(it.published));
            }
            bool dup = false;
            for (int k = 0; k < count; k++) {
                if (strcmp(items[k].video_id, it.video_id) == 0) { dup = true; break; }
            }
            if (!dup && it.title[0]) {
                items[count++] = it;
            }
        }
        pos = cur + 1;
    }
    return count;
}

int yt_parse_player_json(const char *json, size_t len, const char *fallback_vid, yt_player_info_t *info) {
    if (!json || len == 0 || !info) return -1;
    memset(info, 0, sizeof(*info));
    if (fallback_vid) snprintf(info->video_id, sizeof(info->video_id), "%s", fallback_vid);

    size_t root = 0;
    while (root < len && (unsigned char)json[root] <= ' ') root++;
    if (root >= len || json[root] != '{') {
        snprintf(info->error_msg, sizeof(info->error_msg), "Malformed player JSON");
        return -1;
    }
    size_t root_end = json_find_matching(json, len, root, '{', '}');

    size_t ps_s, ps_e;
    if (json_obj_get(json, root, root_end, "playabilityStatus", &ps_s, &ps_e) && json[ps_s] == '{') {
        char st[32] = "";
        size_t vs, ve;
        if (json_obj_get(json, ps_s, ps_e, "status", &vs, &ve)) {
            json_unescape_string(json, vs, ve, st, sizeof(st));
        }
        if (strcmp(st, "OK") != 0) {
            if (json_obj_get(json, ps_s, ps_e, "reason", &vs, &ve)) {
                json_unescape_string(json, vs, ve, info->error_msg, sizeof(info->error_msg));
            } else {
                snprintf(info->error_msg, sizeof(info->error_msg), "Playability: %s", st[0] ? st : "error");
            }
            return -1;
        }
    }

    size_t vd_s, vd_e;
    if (json_obj_get(json, root, root_end, "videoDetails", &vd_s, &vd_e) && json[vd_s] == '{') {
        size_t vs, ve;
        if (json_obj_get(json, vd_s, vd_e, "videoId", &vs, &ve))
            json_unescape_string(json, vs, ve, info->video_id, sizeof(info->video_id));
        if (json_obj_get(json, vd_s, vd_e, "title", &vs, &ve))
            json_unescape_string(json, vs, ve, info->title, sizeof(info->title));
        if (json_obj_get(json, vd_s, vd_e, "author", &vs, &ve))
            json_unescape_string(json, vs, ve, info->author, sizeof(info->author));
        if (json_obj_get(json, vd_s, vd_e, "lengthSeconds", &vs, &ve)) {
            char lbuf[24];
            json_unescape_string(json, vs, ve, lbuf, sizeof(lbuf));
            info->duration_ms = (uint32_t)(strtoul(lbuf, NULL, 10) * 1000u);
        }
    }

    size_t sd_s, sd_e;
    if (json_obj_get(json, root, root_end, "streamingData", &sd_s, &sd_e) && json[sd_s] == '{') {
        size_t fm_s, fm_e;
        if (json_obj_get(json, sd_s, sd_e, "formats", &fm_s, &fm_e) && json[fm_s] == '[') {
            char best_url[4096] = "";
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
                char url_buf[4096] = "";
                char mime[96] = "";
                if (json_obj_get(json, p, ie, "itag", &vs, &ve))
                    itag = (int)strtol(json + vs, NULL, 10);
                if (json_obj_get(json, p, ie, "width", &vs, &ve))
                    w = (int)strtol(json + vs, NULL, 10);
                if (json_obj_get(json, p, ie, "height", &vs, &ve))
                    h = (int)strtol(json + vs, NULL, 10);
                if (json_obj_get(json, p, ie, "fps", &vs, &ve))
                    fps = (int)strtol(json + vs, NULL, 10);
                if (json_obj_get(json, p, ie, "mimeType", &vs, &ve) && json[vs] == '"')
                    json_unescape_string(json, vs, ve, mime, sizeof(mime));
                if (json_obj_get(json, p, ie, "approxDurationMs", &vs, &ve)) {
                    char dbuf[24];
                    json_unescape_string(json, vs, ve, dbuf, sizeof(dbuf));
                    adur = (uint32_t)strtoul(dbuf, NULL, 10);
                }
                if (json_obj_get(json, p, ie, "url", &vs, &ve) && json[vs] == '"') {
                    json_unescape_string(json, vs, ve, url_buf, sizeof(url_buf));
                }

                if (url_buf[0] && (itag == 18 || strstr(mime, "video/mp4") != NULL)) {
                    if (itag == 18 || !best_url[0]) {
                        snprintf(best_url, sizeof(best_url), "%s", url_buf);
                        best_w = w; best_h = h; best_fps = fps; best_dur = adur;
                    }
                    if (itag == 18) break;
                }
                p = ie;
            }
            if (best_url[0]) {
                snprintf(info->stream_url, sizeof(info->stream_url), "%s", best_url);
                info->width = best_w;
                info->height = best_h;
                info->fps = best_fps;
                if (best_dur > 0) info->duration_ms = best_dur;
                return 0;
            }
        }
    }

    if (!info->error_msg[0]) {
        snprintf(info->error_msg, sizeof(info->error_msg), "No progressive MP4 URL (itag 18) in stream");
    }
    return -1;
}

/* =========================================================================
 * Live YouTube Home / Search HTML Builder
 * ========================================================================= */

static void extract_query_param(const char *url, const char *pname, char *out, size_t cap) {
    out[0] = '\0';
    if (!url) return;
    const char *q = strchr(url, '?');
    if (!q) return;
    q++;
    size_t plen = strlen(pname);
    while (*q) {
        const char *amp = strchr(q, '&');
        size_t part_len = amp ? (size_t)(amp - q) : strlen(q);
        const char *eq = memchr(q, '=', part_len);
        if (eq && (size_t)(eq - q) == plen && strncmp(q, pname, plen) == 0) {
            const char *val = eq + 1;
            size_t vlen = (size_t)((q + part_len) - val);
            yt_url_decode(val, vlen, out, cap);
            return;
        }
        if (!amp) break;
        q = amp + 1;
    }
}

static void json_escape_into(const char *src, char *dst, size_t cap) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 3 < cap; i++) {
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

    char query[256] = "";
    extract_query_param(url, "search_query", query, sizeof(query));
    if (!query[0]) extract_query_param(url, "q", query, sizeof(query));
    bool is_home = (query[0] == '\0');
    if (is_home) {
        snprintf(query, sizeof(query), "Blender Open Movie 4K");
    }

    char q_esc[512];
    json_escape_into(query, q_esc, sizeof(q_esc));
    char payload[1024];
    snprintf(payload, sizeof(payload),
             "{\"query\":\"%s\",\"context\":{\"client\":{\"clientName\":\"WEB\","
             "\"clientVersion\":\"2.20240726.00.00\",\"hl\":\"en\",\"gl\":\"US\"}}}",
             q_esc);

    yt_http_stream_t st;
    int rc = yt_http_stream_open(&st, "POST",
        "https://www.youtube.com/youtubei/v1/search?prettyPrint=false",
        "application/json", payload, strlen(payload),
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36",
        false, 0);

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
                        if (cap >= 2u * 1024u * 1024u) break;
                        size_t nc = cap * 2;
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
    yt_http_stream_close(&st);

    yt_search_item_t items[8];
    int nitems = resp ? yt_parse_search_json(resp, rlen, items, 8) : 0;
    if (resp) free(resp);

    str_builder_t sb = { NULL, 0, 0 };
    sb_append(&sb,
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<title>");
    sb_append_html_esc(&sb, is_home ? "YouTube - IPO_OS" : query);
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
    sb_append_html_esc(&sb, is_home ? "" : query);
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
            char qurl[256] = "https://www.youtube.com/results?search_query=";
            url_encode_append(chips[c][1], qurl, sizeof(qurl));
            sb_append(&sb, "<a class=\"chip\" href=\"");
            sb_append_html_esc(&sb, qurl);
            sb_append(&sb, "\">");
            sb_append_html_esc(&sb, chips[c][0]);
            sb_append(&sb, "</a> ");
        }
    }
    sb_append(&sb, "</div></div>");

    if (nitems == 0) {
        serial_printf("[yt_browse] url=%s query=\"%s\" items=0 err=%s\n",
                      url ? url : "", query, st.err_msg);
        sb_append(&sb, "<div class=\"card\"><b>No results loaded");
        if (st.err_msg[0]) {
            sb_append(&sb, " (");
            sb_append_html_esc(&sb, st.err_msg);
            sb_append(&sb, ")");
        }
        sb_append(&sb, ".</b> Try direct demo: "
                  "<a href=\"https://www.youtube.com/watch?v=YE7VzlLtp-4\">Big Buck Bunny (Blender)</a>"
                  "</div>");
    } else {
        serial_printf("[yt_browse] url=%s query=\"%s\" items=%d\n",
                      url ? url : "", query, nitems);
        sb_append(&sb, "<p class=\"meta\">Showing live YouTube results for: <b>");
        sb_append_html_esc(&sb, query);
        sb_append(&sb, "</b></p>");
        for (int i = 0; i < nitems; i++) {
            char watch_url[96];
            char thumb_url[96];
            snprintf(watch_url, sizeof(watch_url), "https://www.youtube.com/watch?v=%s", items[i].video_id);
            snprintf(thumb_url, sizeof(thumb_url), "https://i.ytimg.com/vi/%s/mqdefault.jpg", items[i].video_id);

            sb_append(&sb, "<div class=\"card\"><table border=\"0\" cellpadding=\"3\"><tr>"
                           "<td width=\"116\" valign=\"top\"><a href=\"");
            sb_append(&sb, watch_url);
            sb_append(&sb, "\"><img src=\"");
            sb_append(&sb, thumb_url);
            sb_append(&sb, "\" width=\"112\" height=\"63\" alt=\"thumb\"></a></td><td valign=\"top\">"
                           "<a class=\"vtitle\" href=\"");
            sb_append(&sb, watch_url);
            sb_append(&sb, "\">");
            sb_append_html_esc(&sb, items[i].title);
            sb_append(&sb, "</a><br><span class=\"meta\">");
            if (items[i].channel[0]) {
                sb_append_html_esc(&sb, items[i].channel);
            }
            if (items[i].views[0]) {
                sb_append(&sb, " &bull; ");
                sb_append_html_esc(&sb, items[i].views);
            }
            if (items[i].duration[0]) {
                sb_append(&sb, " &nbsp;<span class=\"dur\">");
                sb_append_html_esc(&sb, items[i].duration);
                sb_append(&sb, "</span>");
            }
            sb_append(&sb, "</span><br><br><a class=\"playbtn\" href=\"");
            sb_append(&sb, watch_url);
            sb_append(&sb, "\">Play Video</a></td></tr></table></div>");
        }
    }

    sb_append(&sb, "</body></html>");
    *out_html = sb.buf;
    *out_len = sb.len;
    return sb.buf ? 0 : -1;
}
