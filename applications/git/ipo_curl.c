#include "compat/curl/curl.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <syscall.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <zlib.h>

#include "../../lib/bearssl/inc/bearssl.h"
#define static_const static const
#include "../links/upstream/certs.inc"
#undef static_const

/* --------------------------------------------------------------------------
 * BearSSL X.509 Trust Anchor Store & Conditional Verification Wrappers
 * -------------------------------------------------------------------------- */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} der_dn_buf_t;

static void dn_append_cb(void *ctx, const void *buf, size_t len) {
    der_dn_buf_t *d = (der_dn_buf_t *)ctx;
    if (!d || !buf || len == 0u) return;
    if (d->len + len > d->cap) {
        size_t ncap = d->cap ? d->cap : 128u;
        while (ncap < d->len + len) {
            if (ncap > SIZE_MAX / 2u) return;
            ncap *= 2u;
        }
        uint8_t *nb = (uint8_t *)realloc(d->data, ncap);
        if (!nb) return;
        d->data = nb;
        d->cap = ncap;
    }
    memcpy(d->data + d->len, buf, len);
    d->len += len;
}

typedef struct {
    br_x509_trust_anchor *items;
    size_t count;
    size_t cap;
} curl_ta_list_t;

static void curl_ta_add_der(curl_ta_list_t *list, const uint8_t *der, size_t der_len) {
    if (!list || !der || der_len == 0u) return;
    br_x509_decoder_context dc;
    der_dn_buf_t dn = { NULL, 0u, 0u };
    br_x509_decoder_init(&dc, dn_append_cb, &dn);
    br_x509_decoder_push(&dc, der, der_len);
    const br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || br_x509_decoder_last_error(&dc) != 0 || dn.len == 0u) {
        free(dn.data);
        return;
    }
    if (list->count >= list->cap) {
        size_t ncap = list->cap ? (list->cap * 2u) : 32u;
        br_x509_trust_anchor *ni = (br_x509_trust_anchor *)realloc(
            list->items, ncap * sizeof(br_x509_trust_anchor));
        if (!ni) {
            free(dn.data);
            return;
        }
        list->items = ni;
        list->cap = ncap;
    }
    br_x509_trust_anchor *ta = &list->items[list->count];
    memset(ta, 0, sizeof(*ta));
    ta->dn.data = dn.data;
    ta->dn.len = dn.len;
    ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0u;
    ta->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        ta->pkey.key.rsa.elen = pk->key.rsa.elen;
        ta->pkey.key.rsa.n = (unsigned char *)malloc(pk->key.rsa.nlen ? pk->key.rsa.nlen : 1u);
        ta->pkey.key.rsa.e = (unsigned char *)malloc(pk->key.rsa.elen ? pk->key.rsa.elen : 1u);
        if (!ta->pkey.key.rsa.n || !ta->pkey.key.rsa.e) {
            free(ta->pkey.key.rsa.n);
            free(ta->pkey.key.rsa.e);
            free(dn.data);
            return;
        }
        if (pk->key.rsa.nlen) memcpy(ta->pkey.key.rsa.n, pk->key.rsa.n, pk->key.rsa.nlen);
        if (pk->key.rsa.elen) memcpy(ta->pkey.key.rsa.e, pk->key.rsa.e, pk->key.rsa.elen);
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.curve = pk->key.ec.curve;
        ta->pkey.key.ec.qlen = pk->key.ec.qlen;
        ta->pkey.key.ec.q = (unsigned char *)malloc(pk->key.ec.qlen ? pk->key.ec.qlen : 1u);
        if (!ta->pkey.key.ec.q) {
            free(dn.data);
            return;
        }
        if (pk->key.ec.qlen) memcpy(ta->pkey.key.ec.q, pk->key.ec.q, pk->key.ec.qlen);
    } else {
        free(dn.data);
        return;
    }
    list->count++;
}

static void curl_ta_free(curl_ta_list_t *list) {
    if (!list) return;
    for (size_t i = 0u; i < list->count; i++) {
        free(list->items[i].dn.data);
        if (list->items[i].pkey.key_type == BR_KEYTYPE_RSA) {
            free(list->items[i].pkey.key.rsa.n);
            free(list->items[i].pkey.key.rsa.e);
        } else if (list->items[i].pkey.key_type == BR_KEYTYPE_EC) {
            free(list->items[i].pkey.key.ec.q);
        }
    }
    free(list->items);
    list->items = NULL;
    list->count = 0u;
    list->cap = 0u;
}

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    int in_cert;
} pem_der_acc_t;

static void pem_obj_bytes_cb(void *ctx, const void *buf, size_t len) {
    pem_der_acc_t *a = (pem_der_acc_t *)ctx;
    if (!a || !a->in_cert || !buf || len == 0u) return;
    if (a->len + len > a->cap) {
        size_t ncap = a->cap ? a->cap : 2048u;
        while (ncap < a->len + len) {
            if (ncap > SIZE_MAX / 2u) return;
            ncap *= 2u;
        }
        uint8_t *nb = (uint8_t *)realloc(a->buf, ncap);
        if (!nb) return;
        a->buf = nb;
        a->cap = ncap;
    }
    memcpy(a->buf + a->len, buf, len);
    a->len += len;
}

static int curl_ta_load_pem_file(curl_ta_list_t *list, const char *path) {
    if (!path || !*path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    pem_der_acc_t acc = { NULL, 0u, 0u, 0 };
    br_pem_decoder_setdest(&pc, pem_obj_bytes_cb, &acc);

    uint8_t chunk[1024];
    size_t nread;
    while ((nread = fread(chunk, 1u, sizeof(chunk), f)) > 0u) {
        size_t off = 0u;
        while (off < nread) {
            size_t used = br_pem_decoder_push(&pc, chunk + off, nread - off);
            off += used;
            int ev = br_pem_decoder_event(&pc);
            if (ev == BR_PEM_BEGIN_OBJ) {
                const char *name = br_pem_decoder_name(&pc);
                acc.len = 0u;
                acc.in_cert = (name && (strcmp(name, "CERTIFICATE") == 0 ||
                                        strcmp(name, "X509 CERTIFICATE") == 0 ||
                                        strcmp(name, "TRUSTED CERTIFICATE") == 0));
            } else if (ev == BR_PEM_END_OBJ) {
                if (acc.in_cert && acc.len > 0u) {
                    curl_ta_add_der(list, acc.buf, acc.len);
                }
                acc.in_cert = 0;
                acc.len = 0u;
            } else if (ev == BR_PEM_ERROR) {
                acc.in_cert = 0;
                acc.len = 0u;
            }
        }
    }
    fclose(f);
    free(acc.buf);
    return (list->count > 0u) ? 0 : -1;
}

static pthread_mutex_t s_builtin_ta_lock = PTHREAD_MUTEX_INITIALIZER;
static curl_ta_list_t s_builtin_ta = { NULL, 0u, 0u };
static int s_builtin_ta_loaded = 0;

static const curl_ta_list_t *get_builtin_trust_anchors(void) {
    pthread_mutex_lock(&s_builtin_ta_lock);
    if (!s_builtin_ta_loaded) {
        size_t n = sizeof(certificates) / sizeof(certificates[0]);
        for (size_t i = 0u; i < n; i++) {
            if (certificates[i].data && certificates[i].len > 0) {
                curl_ta_add_der(&s_builtin_ta,
                                (const uint8_t *)certificates[i].data,
                                (size_t)certificates[i].len);
            }
        }
        s_builtin_ta_loaded = 1;
    }
    pthread_mutex_unlock(&s_builtin_ta_lock);
    return &s_builtin_ta;
}

typedef struct {
    const br_x509_class *vtable;
    const br_x509_class **inner;
    int allow_untrusted;
    int allow_bad_host;
} x509_policy_context;

static void xwc_start_chain(const br_x509_class **ctx, const char *server_name) {
    x509_policy_context *xwc = (x509_policy_context *)ctx;
    (*xwc->inner)->start_chain(xwc->inner, server_name);
}

static void xwc_start_cert(const br_x509_class **ctx, uint32_t length) {
    x509_policy_context *xwc = (x509_policy_context *)ctx;
    (*xwc->inner)->start_cert(xwc->inner, length);
}

static void xwc_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    x509_policy_context *xwc = (x509_policy_context *)ctx;
    (*xwc->inner)->append(xwc->inner, buf, len);
}

static void xwc_end_cert(const br_x509_class **ctx) {
    x509_policy_context *xwc = (x509_policy_context *)ctx;
    (*xwc->inner)->end_cert(xwc->inner);
}

static unsigned xwc_end_chain(const br_x509_class **ctx) {
    x509_policy_context *xwc = (x509_policy_context *)ctx;
    unsigned r = (*xwc->inner)->end_chain(xwc->inner);
    if (xwc->allow_untrusted) {
        if (r == BR_ERR_X509_NOT_TRUSTED || r == BR_ERR_X509_EXPIRED ||
            r == BR_ERR_X509_TIME_UNKNOWN || r == BR_ERR_X509_BAD_SERVER_NAME) {
            r = 0u;
        }
    } else if (xwc->allow_bad_host && r == BR_ERR_X509_BAD_SERVER_NAME) {
        r = 0u;
    }
    return r;
}

static const br_x509_pkey *xwc_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    x509_policy_context *xwc = (x509_policy_context *)ctx;
    return (*xwc->inner)->get_pkey(xwc->inner, usages);
}

static const br_x509_class x509_policy_vtable = {
    sizeof(x509_policy_context),
    xwc_start_chain,
    xwc_start_cert,
    xwc_append,
    xwc_end_cert,
    xwc_end_chain,
    xwc_get_pkey
};

/* --------------------------------------------------------------------------
 * Raw kernel network socket & TLS transport
 * -------------------------------------------------------------------------- */
typedef struct {
    int sock;
    int use_tls;
    int peer_eof;
    int last_tls_err;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    x509_policy_context xwc;
    curl_ta_list_t custom_tas;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
} ipo_net_conn_t;

static int raw_net_socket(void) {
    uint32_t args[3] = { AF_INET, SOCK_STREAM, 0 };
    return ipo_syscall(IPO_SYSCALL_NET_SOCKET, 3u, args);
}

static int raw_net_connect_endpoint(int sock, const char *host, uint16_t port) {
    uint32_t args[3] = {
        (uint32_t)sock,
        (uint32_t)(uintptr_t)host,
        (uint32_t)port
    };
    return ipo_syscall(IPO_SYSCALL_NET_CONNECT_ENDPOINT, 3u, args);
}

static int raw_net_send(int sock, const void *buf, size_t len) {
    uint32_t args[4] = {
        (uint32_t)sock,
        (uint32_t)(uintptr_t)buf,
        (uint32_t)len,
        0u
    };
    return ipo_syscall(IPO_SYSCALL_NET_SEND, 4u, args);
}

static int raw_net_recv(int sock, void *buf, size_t len) {
    uint32_t args[4] = {
        (uint32_t)sock,
        (uint32_t)(uintptr_t)buf,
        (uint32_t)len,
        0u
    };
    return ipo_syscall(IPO_SYSCALL_NET_RECV, 4u, args);
}

static void raw_net_close(int sock) {
    if (sock >= 0) {
        uint32_t args[1] = { (uint32_t)sock };
        ipo_syscall(IPO_SYSCALL_NET_CLOSE, 1u, args);
    }
}

static void raw_net_poll(void) {
    ipo_syscall(IPO_SYSCALL_NET_POLL, 0u, NULL);
}

static uint32_t raw_millis(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
}

static int raw_send_all(int sock, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint32_t last_ms = raw_millis();
    while (len > 0) {
        raw_net_poll();
        int w = raw_net_send(sock, p, len);
        if (w > 0) {
            p += (size_t)w;
            len -= (size_t)w;
            last_ms = raw_millis();
        } else {
            if (raw_millis() - last_ms > 20000u) return -1;
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
        }
    }
    return 0;
}

static int pump_tls_until(ipo_net_conn_t *conn, unsigned target_mask) {
    uint32_t last_ms = raw_millis();
    for (;;) {
        raw_net_poll();
        unsigned st = br_ssl_engine_current_state(&conn->sc.eng);
        if (st & BR_SSL_SENDREC) {
            size_t len = 0;
            unsigned char *buf = br_ssl_engine_sendrec_buf(&conn->sc.eng, &len);
            if (buf && len > 0) {
                int w = raw_net_send(conn->sock, buf, len);
                if (w > 0) {
                    br_ssl_engine_sendrec_ack(&conn->sc.eng, (size_t)w);
                    last_ms = raw_millis();
                    continue;
                }
            }
        }
        st = br_ssl_engine_current_state(&conn->sc.eng);
        if (st & target_mask) {
            return 1;
        }
        if (st == BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(&conn->sc.eng);
            return (err == BR_ERR_OK) ? 0 : -1;
        }
        if (st & BR_SSL_RECVREC) {
            if (conn->peer_eof) {
                return 0;
            }
            size_t len = 0;
            unsigned char *buf = br_ssl_engine_recvrec_buf(&conn->sc.eng, &len);
            if (buf && len > 0) {
                int r = raw_net_recv(conn->sock, buf, len);
                if (r > 0) {
                    br_ssl_engine_recvrec_ack(&conn->sc.eng, (size_t)r);
                    last_ms = raw_millis();
                    continue;
                } else if (r == 0) {
                    conn->peer_eof = 1;
                    if (br_ssl_engine_current_state(&conn->sc.eng) & target_mask)
                        return 1;
                    return 0;
                }
            }
        }
        if (raw_millis() - last_ms > 25000u) {
            return -1;
        }
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }
}

static int conn_connect(ipo_net_conn_t *conn, const char *host, uint16_t port, int use_tls,
                        long ssl_verifypeer, long ssl_verifyhost, const char *cainfo) {
    memset(conn, 0, sizeof(*conn));
    conn->sock = raw_net_socket();
    if (conn->sock < 0) return -1;
    if (raw_net_connect_endpoint(conn->sock, host, port) < 0) {
        raw_net_close(conn->sock);
        conn->sock = -1;
        return -1;
    }
    conn->use_tls = use_tls;
    if (use_tls) {
        const br_x509_trust_anchor *tas = NULL;
        size_t ta_count = 0u;
        const char *no_verify_env = getenv("GIT_SSL_NO_VERIFY");
        if (no_verify_env && (strcmp(no_verify_env, "1") == 0 ||
                              strcasecmp(no_verify_env, "true") == 0)) {
            ssl_verifypeer = 0L;
        }

        if (ssl_verifypeer != 0L) {
            const char *ca_path = cainfo;
            if (!ca_path || !*ca_path) ca_path = getenv("GIT_SSL_CAINFO");
            if (!ca_path || !*ca_path) ca_path = getenv("SSL_CERT_FILE");
            if (ca_path && *ca_path) {
                if (curl_ta_load_pem_file(&conn->custom_tas, ca_path) != 0) {
                    raw_net_close(conn->sock);
                    conn->sock = -1;
                    return -2;
                }
                tas = conn->custom_tas.items;
                ta_count = conn->custom_tas.count;
            } else if (curl_ta_load_pem_file(&conn->custom_tas, "/etc/ssl/certs/ca-certificates.crt") == 0 ||
                       curl_ta_load_pem_file(&conn->custom_tas, "/data/ssl/ca-certificates.crt") == 0) {
                tas = conn->custom_tas.items;
                ta_count = conn->custom_tas.count;
            } else {
                const curl_ta_list_t *builtin = get_builtin_trust_anchors();
                tas = builtin->items;
                ta_count = builtin->count;
            }
        }

        br_ssl_client_init_full(&conn->sc, &conn->xc, tas, ta_count);
        time_t now = time(NULL);
        if (now > 1577836800) {
            uint32_t days = (uint32_t)(now / 86400) + 719528u;
            uint32_t secs = (uint32_t)(now % 86400);
            br_x509_minimal_set_time(&conn->xc, days, secs);
        }

        if (ssl_verifypeer == 0L || ssl_verifyhost == 0L) {
            conn->xwc.vtable = &x509_policy_vtable;
            conn->xwc.inner = &conn->xc.vtable;
            conn->xwc.allow_untrusted = (ssl_verifypeer == 0L);
            conn->xwc.allow_bad_host = (ssl_verifyhost == 0L);
            br_ssl_engine_set_x509(&conn->sc.eng, &conn->xwc.vtable);
        } else {
            br_ssl_engine_set_x509(&conn->sc.eng, &conn->xc.vtable);
        }

        br_ssl_engine_set_buffer(&conn->sc.eng, conn->iobuf, sizeof(conn->iobuf), 1);
        unsigned char seed[32];
        if (getrandom(seed, sizeof(seed), 0) == (ssize_t)sizeof(seed)) {
            br_ssl_engine_inject_entropy(&conn->sc.eng, seed, sizeof(seed));
        }
        br_ssl_client_reset(&conn->sc, host, 0);
        if (pump_tls_until(conn, BR_SSL_SENDAPP | BR_SSL_RECVAPP) <= 0) {
            int err = br_ssl_engine_last_error(&conn->sc.eng);
            conn->last_tls_err = err;
            curl_ta_free(&conn->custom_tas);
            raw_net_close(conn->sock);
            conn->sock = -1;
            if (err >= BR_ERR_X509_OK && err <= BR_ERR_X509_LIMIT_EXCEEDED) {
                return -2;
            }
            return -1;
        }
    }
    return 0;
}

static int conn_write_all(ipo_net_conn_t *conn, const void *data, size_t len) {
    if (!conn->use_tls) {
        return raw_send_all(conn->sock, data, len);
    }
    const unsigned char *p = (const unsigned char *)data;
    while (len > 0) {
        if (pump_tls_until(conn, BR_SSL_SENDAPP) <= 0) {
            return -1;
        }
        size_t alen = 0;
        unsigned char *abuf = br_ssl_engine_sendapp_buf(&conn->sc.eng, &alen);
        if (!abuf || alen == 0) return -1;
        size_t chunk = len < alen ? len : alen;
        memcpy(abuf, p, chunk);
        br_ssl_engine_sendapp_ack(&conn->sc.eng, chunk);
        p += chunk;
        len -= chunk;
    }
    br_ssl_engine_flush(&conn->sc.eng, 0);
    /* Flush any pending records */
    uint32_t last_ms = raw_millis();
    while (br_ssl_engine_current_state(&conn->sc.eng) & BR_SSL_SENDREC) {
        raw_net_poll();
        size_t slen = 0;
        unsigned char *sbuf = br_ssl_engine_sendrec_buf(&conn->sc.eng, &slen);
        if (!sbuf || slen == 0) break;
        int w = raw_net_send(conn->sock, sbuf, slen);
        if (w > 0) {
            br_ssl_engine_sendrec_ack(&conn->sc.eng, (size_t)w);
            last_ms = raw_millis();
        } else {
            if (raw_millis() - last_ms > 20000u) return -1;
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
        }
    }
    return 0;
}

static ssize_t conn_read_some(ipo_net_conn_t *conn, void *buf, size_t max_len) {
    if (!conn->use_tls) {
        uint32_t last_ms = raw_millis();
        for (;;) {
            raw_net_poll();
            int r = raw_net_recv(conn->sock, buf, max_len);
            if (r > 0) return (ssize_t)r;
            if (r == 0) return 0;
            if (raw_millis() - last_ms > 25000u) return -1;
            ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
        }
    }
    int rc = pump_tls_until(conn, BR_SSL_RECVAPP);
    unsigned st = br_ssl_engine_current_state(&conn->sc.eng);
    if (st & BR_SSL_RECVAPP) {
        size_t alen = 0;
        unsigned char *abuf = br_ssl_engine_recvapp_buf(&conn->sc.eng, &alen);
        if (abuf && alen > 0) {
            size_t to_copy = max_len < alen ? max_len : alen;
            memcpy(buf, abuf, to_copy);
            br_ssl_engine_recvapp_ack(&conn->sc.eng, to_copy);
            return (ssize_t)to_copy;
        }
    }
    return (rc <= 0) ? (ssize_t)rc : 0;
}

static void conn_close(ipo_net_conn_t *conn) {
    if (!conn) return;
    if (conn->use_tls) {
        br_ssl_engine_close(&conn->sc.eng);
    }
    curl_ta_free(&conn->custom_tas);
    if (conn->sock >= 0) {
        raw_net_close(conn->sock);
        conn->sock = -1;
    }
}

/* --------------------------------------------------------------------------
 * Easy / Multi Handle Definitions
 * -------------------------------------------------------------------------- */
typedef struct ipo_curl_easy {
    char *url;
    char *effective_url;
    char *useragent;
    char *custom_request;
    char *username;
    char *password;
    char *userpwd;
    char *range;
    char *accept_encoding;
    char *errorbuf;
    char *cainfo;
    char *capath;

    const void *postfields;
    long postfieldsize;
    curl_off_t postfieldsize_large;

    long post;
    long nobody;
    long httpget;
    long upload;
    long failonerror;
    long follow_location;
    long maxredirs;
    long httpauth;
    long verbose;
    long ssl_verifypeer;
    long ssl_verifyhost;

    struct curl_slist *headers;

    curl_write_callback write_fn;
    void *write_data;
    curl_write_callback header_fn;
    void *header_data;
    curl_read_callback read_fn;
    void *infile;
    curl_seek_callback seek_fn;
    void *seek_data;
    curl_debug_callback debug_fn;
    void *debug_data;
    void *private_data;

    /* Response info */
    long response_code;
    long connect_code;
    long auth_avail;
    char *content_type;
} ipo_curl_easy_t;

typedef struct {
    ipo_curl_easy_t **handles;
    int *states; /* 0=unused, 1=pending, 2=done_unread, 3=done_read */
    CURLcode *results;
    int cap;
    CURLMsg last_msg;
} ipo_curl_multi_t;

/* --------------------------------------------------------------------------
 * Base64 helper for HTTP Basic Auth
 * -------------------------------------------------------------------------- */
static char *base64_encode_str(const char *input) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(input);
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = i < len ? (unsigned char)input[i++] : 0;
        uint32_t b = i < len ? (unsigned char)input[i++] : 0;
        uint32_t c = i < len ? (unsigned char)input[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = b64[(triple >> 18) & 0x3F];
        out[j++] = b64[(triple >> 12) & 0x3F];
        out[j++] = b64[(triple >> 6) & 0x3F];
        out[j++] = b64[triple & 0x3F];
    }
    size_t mod = len % 3;
    if (mod == 1) {
        out[out_len - 1] = '=';
        out[out_len - 2] = '=';
    } else if (mod == 2) {
        out[out_len - 1] = '=';
    }
    out[out_len] = '\0';
    return out;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_percent_decode(char *s) {
    char *dst = s;
    for (const char *src = s; *src; src++) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = hex_val(src[1]);
            int lo = hex_val(src[2]);
            if (hi >= 0 && lo >= 0) {
                *dst++ = (char)((hi << 4) | lo);
                src += 2;
                continue;
            }
        }
        *dst++ = *src;
    }
    *dst = '\0';
}

typedef struct {
    int use_tls;
    char *host;
    uint16_t port;
    char *path;
    char *user;
    char *pass;
    int has_auth;
} parsed_url_t;

static void free_parsed_url(parsed_url_t *u) {
    if (!u) return;
    free(u->host);
    free(u->path);
    free(u->user);
    free(u->pass);
    memset(u, 0, sizeof(*u));
}

static char *dup_substr(const char *s, size_t len) {
    char *p = (char *)malloc(len + 1);
    if (!p) return NULL;
    if (len > 0) memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static int parse_http_url(const char *url, parsed_url_t *out) {
    memset(out, 0, sizeof(*out));
    if (!url) return -1;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        out->use_tls = 1;
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        out->use_tls = 0;
        out->port = 80;
        p += 7;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    const char *authority_end = slash ? slash : (p + strlen(p));
    const char *at = memchr(p, '@', (size_t)(authority_end - p));
    const char *host_start = p;
    if (at) {
        size_t ulen = (size_t)(at - p);
        const char *colon = memchr(p, ':', ulen);
        if (colon) {
            size_t nlen = (size_t)(colon - p);
            size_t plen = (size_t)(at - (colon + 1));
            out->user = dup_substr(p, nlen);
            out->pass = dup_substr(colon + 1, plen);
            if (!out->user || !out->pass) {
                free_parsed_url(out);
                return -1;
            }
        } else {
            out->user = dup_substr(p, ulen);
            out->pass = dup_substr("", 0);
            if (!out->user || !out->pass) {
                free_parsed_url(out);
                return -1;
            }
        }
        url_percent_decode(out->user);
        url_percent_decode(out->pass);
        out->has_auth = 1;
        host_start = at + 1;
    }

    size_t host_port_len = (size_t)(authority_end - host_start);
    const char *port_colon = memchr(host_start, ':', host_port_len);
    if (port_colon) {
        size_t hlen = (size_t)(port_colon - host_start);
        if (hlen == 0) {
            free_parsed_url(out);
            return -1;
        }
        out->host = dup_substr(host_start, hlen);
        int pv = atoi(port_colon + 1);
        if (pv > 0 && pv <= 65535) out->port = (uint16_t)pv;
    } else {
        if (host_port_len == 0) {
            free_parsed_url(out);
            return -1;
        }
        out->host = dup_substr(host_start, host_port_len);
    }

    out->path = strdup(slash ? slash : "/");
    if (!out->host || !out->path) {
        free_parsed_url(out);
        return -1;
    }
    return 0;
}

static char *resolve_redirect_url(const parsed_url_t *base, const char *location) {
    if (!location || !*location) return NULL;
    if (strncmp(location, "http://", 7) == 0 || strncmp(location, "https://", 8) == 0) {
        return strdup(location);
    }
    const char *scheme = base->use_tls ? "https" : "http";
    int default_port = base->use_tls ? 443 : 80;
    size_t cap = strlen(location) + strlen(base->host) + 64;
    char *res = (char *)malloc(cap);
    if (!res) return NULL;
    if (location[0] == '/') {
        if (base->port == default_port)
            snprintf(res, cap, "%s://%s%s", scheme, base->host, location);
        else
            snprintf(res, cap, "%s://%s:%u%s", scheme, base->host, base->port, location);
    } else {
        if (base->port == default_port)
            snprintf(res, cap, "%s://%s/%s", scheme, base->host, location);
        else
            snprintf(res, cap, "%s://%s:%u/%s", scheme, base->host, base->port, location);
    }
    return res;
}

/* --------------------------------------------------------------------------
 * Dynamic byte buffer helper
 * -------------------------------------------------------------------------- */
typedef struct {
    unsigned char *data;
    size_t len;
    size_t cap;
} byte_buf_t;

static int bb_reserve(byte_buf_t *b, size_t needed) {
    if (needed <= b->cap) return 0;
    size_t new_cap = b->cap ? b->cap : 4096;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) { new_cap = needed; break; }
        new_cap *= 2;
    }
    unsigned char *nd = (unsigned char *)realloc(b->data, new_cap);
    if (!nd) return -1;
    b->data = nd;
    b->cap = new_cap;
    return 0;
}

static int bb_append(byte_buf_t *b, const void *src, size_t len) {
    if (len == 0) return 0;
    if (len > SIZE_MAX - b->len - 1) return -1;
    if (bb_reserve(b, b->len + len + 1) < 0) return -1;
    memcpy(b->data + b->len, src, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

static int bb_append_str(byte_buf_t *b, const char *s) {
    return bb_append(b, s, strlen(s));
}

static int bb_append_fmt(byte_buf_t *b, const char *fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0) {
        va_end(ap);
        return -1;
    }
    if ((size_t)n > SIZE_MAX - b->len - 1 || bb_reserve(b, b->len + (size_t)n + 1) < 0) {
        va_end(ap);
        return -1;
    }
    vsnprintf((char *)(b->data + b->len), (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
    b->data[b->len] = '\0';
    return 0;
}

static void bb_free(byte_buf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* --------------------------------------------------------------------------
 * Decompress gzip/deflate response body if needed
 * -------------------------------------------------------------------------- */
static int decompress_gzip_buf(const unsigned char *in_data, size_t in_len, byte_buf_t *out_buf) {
    if (in_len == 0) return 0;
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.next_in = (Bytef *)in_data;
    zs.avail_in = (uInt)in_len;
    if (inflateInit2(&zs, 15 + 32) != Z_OK) {
        return -1;
    }
    unsigned char chunk[16384];
    int ret;
    do {
        zs.next_out = chunk;
        zs.avail_out = sizeof(chunk);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END && ret != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return -1;
        }
        size_t have = sizeof(chunk) - zs.avail_out;
        if (have > 0 && bb_append(out_buf, chunk, have) < 0) {
            inflateEnd(&zs);
            return -1;
        }
    } while (ret != Z_STREAM_END && zs.avail_in > 0);
    inflateEnd(&zs);
    return 0;
}

/* --------------------------------------------------------------------------
 * Core HTTP/1.1 & HTTPS Request Execution
 * -------------------------------------------------------------------------- */
static size_t default_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    FILE *fp = userdata ? (FILE *)userdata : stdout;
    return fwrite(ptr, size, nmemb, fp);
}

static void set_err(ipo_curl_easy_t *h, const char *fmt, ...) {
    if (!h->errorbuf) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->errorbuf, CURL_ERROR_SIZE, fmt, ap);
    va_end(ap);
}


static int strncasestr_check(const char *haystack, const char *needle) {
    if (!haystack || !needle) return 0;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, nlen) == 0) return 1;
    }
    return 0;
}

CURLcode curl_easy_perform(CURL *curl) {
    ipo_curl_easy_t *h = (ipo_curl_easy_t *)curl;
    if (!h || !h->url) return CURLE_URL_MALFORMAT;
    if (h->errorbuf) h->errorbuf[0] = '\0';

    free(h->effective_url);
    h->effective_url = strdup(h->url);
    free(h->content_type);
    h->content_type = NULL;
    h->response_code = 0;
    h->connect_code = 0;
    h->auth_avail = 0;

    int redirects = 0;
    long max_redirs = (h->maxredirs > 0) ? h->maxredirs : 20;

    while (1) {
        parsed_url_t purl;
        if (parse_http_url(h->effective_url, &purl) < 0) {
            set_err(h, "Malformed URL '%s'", h->effective_url ? h->effective_url : "");
            return CURLE_URL_MALFORMAT;
        }

        const char *method = "GET";
        if (h->custom_request && h->custom_request[0]) {
            method = h->custom_request;
        } else if (h->nobody) {
            method = "HEAD";
        } else if (h->post || h->postfields) {
            method = "POST";
        } else if (h->upload) {
            method = "PUT";
        }

        /* Check custom headers */
        int has_user_agent = 0;
        int has_auth_hdr = 0;
        int has_content_type = 0;
        int has_content_length = 0;
        int has_chunked_te = 0;
        int has_host_hdr = 0;

        for (struct curl_slist *s = h->headers; s; s = s->next) {
            if (!s->data) continue;
            if (strncasecmp(s->data, "User-Agent:", 11) == 0) has_user_agent = 1;
            else if (strncasecmp(s->data, "Authorization:", 14) == 0) has_auth_hdr = 1;
            else if (strncasecmp(s->data, "Content-Type:", 13) == 0) has_content_type = 1;
            else if (strncasecmp(s->data, "Content-Length:", 15) == 0) has_content_length = 1;
            else if (strncasecmp(s->data, "Host:", 5) == 0) has_host_hdr = 1;
            else if (strncasecmp(s->data, "Transfer-Encoding:", 18) == 0 &&
                     strstr(s->data + 18, "chunked")) {
                has_chunked_te = 1;
            }
        }

        /* Prepare request body if any */
        byte_buf_t body_buf = { NULL, 0, 0 };
        const void *body_ptr = NULL;
        size_t body_len = 0;

        if (h->postfields) {
            body_ptr = h->postfields;
            if (h->postfieldsize_large >= 0) {
                body_len = (size_t)h->postfieldsize_large;
            } else if (h->postfieldsize >= 0) {
                body_len = (size_t)h->postfieldsize;
            } else {
                body_len = strlen((const char *)h->postfields);
            }
        } else if ((h->post || h->upload) && h->read_fn) {
            char chunk[16384];
            for (;;) {
                size_t n = h->read_fn(chunk, 1, sizeof(chunk), h->infile);
                if (n == 0) break;
                if (has_chunked_te) {
                    bb_append_fmt(&body_buf, "%zx\r\n", n);
                    bb_append(&body_buf, chunk, n);
                    bb_append_str(&body_buf, "\r\n");
                } else {
                    bb_append(&body_buf, chunk, n);
                }
            }
            if (has_chunked_te) {
                bb_append_str(&body_buf, "0\r\n\r\n");
            }
            body_ptr = body_buf.data;
            body_len = body_buf.len;
        }

        /* Build HTTP request headers */
        byte_buf_t req_hdr = { NULL, 0, 0 };
        bb_append_fmt(&req_hdr, "%s %s HTTP/1.1\r\n", method, purl.path);

        if (!has_host_hdr) {
            int def_port = purl.use_tls ? 443 : 80;
            if (purl.port == def_port)
                bb_append_fmt(&req_hdr, "Host: %s\r\n", purl.host);
            else
                bb_append_fmt(&req_hdr, "Host: %s:%u\r\n", purl.host, purl.port);
        }

        if (!has_user_agent) {
            bb_append_fmt(&req_hdr, "User-Agent: %s\r\n",
                          (h->useragent && h->useragent[0]) ? h->useragent : "git/2.53.0");
        }

        bb_append_str(&req_hdr, "Connection: close\r\n");

        if (h->range && h->range[0]) {
            bb_append_fmt(&req_hdr, "Range: bytes=%s\r\n", h->range);
        }

        if (!has_auth_hdr) {
            char *cred_str = NULL;
            if (h->userpwd && h->userpwd[0]) {
                cred_str = strdup(h->userpwd);
            } else if ((h->username && h->username[0]) || (h->password && h->password[0])) {
                const char *u = h->username ? h->username : "";
                const char *pw = h->password ? h->password : "";
                size_t clen = strlen(u) + 1 + strlen(pw) + 1;
                cred_str = (char *)malloc(clen);
                if (cred_str) snprintf(cred_str, clen, "%s:%s", u, pw);
            } else if (purl.has_auth) {
                const char *pu = purl.user ? purl.user : "";
                const char *pp = purl.pass ? purl.pass : "";
                size_t clen = strlen(pu) + 1 + strlen(pp) + 1;
                cred_str = (char *)malloc(clen);
                if (cred_str) snprintf(cred_str, clen, "%s:%s", pu, pp);
            }
            if (cred_str) {
                char *b64 = base64_encode_str(cred_str);
                free(cred_str);
                if (b64) {
                    bb_append_fmt(&req_hdr, "Authorization: Basic %s\r\n", b64);
                    free(b64);
                }
            }
        }

        for (struct curl_slist *s = h->headers; s; s = s->next) {
            if (!s->data || !s->data[0]) continue;
            const char *colon = strchr(s->data, ':');
            if (!colon) continue;
            /* Skip empty headers like "Expect:" used by libcurl to disable a default header */
            const char *val = colon + 1;
            while (*val == ' ' || *val == '\t') val++;
            if (*val == '\0') continue;
            /* Ignore "Expect: 100-continue" since we send body immediately */
            if (strncasecmp(s->data, "Expect:", 7) == 0) continue;
            bb_append_fmt(&req_hdr, "%s\r\n", s->data);
        }

        if (!has_chunked_te && !has_content_length) {
            if (body_len > 0 || h->post || h->upload) {
                bb_append_fmt(&req_hdr, "Content-Length: %zu\r\n", body_len);
            }
        }

        bb_append_str(&req_hdr, "\r\n");

        /* Connect and send request */
        ipo_net_conn_t conn;
        int cr = conn_connect(&conn, purl.host, purl.port, purl.use_tls,
                              h->ssl_verifypeer, h->ssl_verifyhost, h->cainfo);
        if (cr < 0) {
            set_err(h, (cr == -2) ? "SSL certificate problem for %s" : "Failed to connect to %s port %u",
                    purl.host, purl.port);
            free_parsed_url(&purl);
            bb_free(&req_hdr);
            bb_free(&body_buf);
            return (cr == -2) ? CURLE_SSL_CACERT : CURLE_COULDNT_CONNECT;
        }

        if (conn_write_all(&conn, req_hdr.data, req_hdr.len) < 0 ||
            (body_len > 0 && conn_write_all(&conn, body_ptr, body_len) < 0)) {
            set_err(h, "Failed to send HTTP request to %s", purl.host);
            conn_close(&conn);
            free_parsed_url(&purl);
            bb_free(&req_hdr);
            bb_free(&body_buf);
            return CURLE_SEND_ERROR;
        }
        bb_free(&req_hdr);
        bb_free(&body_buf);

        /* Receive entire HTTP response */
        byte_buf_t raw_resp = { NULL, 0, 0 };
        unsigned char rbuf[8192];
        for (;;) {
            ssize_t r = conn_read_some(&conn, rbuf, sizeof(rbuf));
            if (r > 0) {
                if (bb_append(&raw_resp, rbuf, (size_t)r) < 0) {
                    conn_close(&conn);
                    free_parsed_url(&purl);
                    bb_free(&raw_resp);
                    return CURLE_OUT_OF_MEMORY;
                }
            } else if (r == 0) {
                break;
            } else {
                conn_close(&conn);
                free_parsed_url(&purl);
                bb_free(&raw_resp);
                set_err(h, "Network read error from %s", purl.host);
                return CURLE_RECV_ERROR;
            }
        }
        conn_close(&conn);

        if (raw_resp.len == 0) {
            free_parsed_url(&purl);
            bb_free(&raw_resp);
            set_err(h, "Empty reply from server");
            return CURLE_GOT_NOTHING;
        }

        /* Split headers and body (skip any 100 Continue interim headers) */
        size_t hdr_start = 0;
        size_t body_offset = 0;
        long status_code = 0;

        while (hdr_start < raw_resp.len) {
            unsigned char *base = raw_resp.data + hdr_start;
            size_t rem = raw_resp.len - hdr_start;
            size_t sep_pos = 0;
            size_t sep_len = 0;
            for (size_t i = 0; i + 1 < rem; i++) {
                if (i + 3 < rem && base[i] == '\r' && base[i + 1] == '\n' &&
                    base[i + 2] == '\r' && base[i + 3] == '\n') {
                    sep_pos = i;
                    sep_len = 4;
                    break;
                }
                if (base[i] == '\n' && base[i + 1] == '\n') {
                    sep_pos = i;
                    sep_len = 2;
                    break;
                }
            }
            if (sep_len == 0) {
                sep_pos = rem;
                sep_len = 0;
            }
            if (rem >= 12 && strncmp((const char *)base, "HTTP/", 5) == 0) {
                const char *sp = (const char *)memchr(base, ' ', sep_pos);
                if (sp) status_code = strtol(sp + 1, NULL, 10);
            }
            if (status_code == 100 && sep_len > 0) {
                hdr_start += sep_pos + sep_len;
                status_code = 0;
                continue;
            }
            body_offset = hdr_start + sep_pos + sep_len;
            break;
        }

        h->response_code = status_code;

        /* Parse headers line by line */
        int resp_chunked = 0;
        int resp_gzip = 0;
        char *location_hdr = NULL;
        size_t pos = hdr_start;
        size_t hdr_end = body_offset;

        while (pos < hdr_end) {
            size_t line_end = pos;
            while (line_end < hdr_end && raw_resp.data[line_end] != '\n') {
                line_end++;
            }
            if (line_end < hdr_end) line_end++;
            size_t line_len = line_end - pos;
            const char *line_ptr = (const char *)(raw_resp.data + pos);

            if (h->header_fn) {
                void *hdata = h->header_data ? h->header_data : h->write_data;
                h->header_fn((char *)line_ptr, 1, line_len, hdata);
            }

            /* Extract trimmed header value */
            char *hline = (char *)malloc(line_len + 1);
            if (!hline) {
                free(location_hdr);
                free_parsed_url(&purl);
                bb_free(&raw_resp);
                return CURLE_OUT_OF_MEMORY;
            }
            memcpy(hline, line_ptr, line_len);
            size_t clen = line_len;
            hline[clen] = '\0';
            while (clen > 0 && (hline[clen - 1] == '\r' || hline[clen - 1] == '\n' || hline[clen - 1] == ' ')) {
                hline[--clen] = '\0';
            }

            if (strncasecmp(hline, "Content-Type:", 13) == 0) {
                const char *v = hline + 13;
                while (*v == ' ' || *v == '\t') v++;
                free(h->content_type);
                h->content_type = strdup(v);
            } else if (strncasecmp(hline, "Location:", 9) == 0) {
                const char *v = hline + 9;
                while (*v == ' ' || *v == '\t') v++;
                free(location_hdr);
                location_hdr = strdup(v);
            } else if (strncasecmp(hline, "Transfer-Encoding:", 18) == 0) {
                if (strstr(hline + 18, "chunked") || strstr(hline + 18, "CHUNKED")) {
                    resp_chunked = 1;
                }
            } else if (strncasecmp(hline, "Content-Encoding:", 17) == 0) {
                if (strstr(hline + 17, "gzip") || strstr(hline + 17, "deflate")) {
                    resp_gzip = 1;
                }
            } else if (strncasecmp(hline, "WWW-Authenticate:", 17) == 0) {
                if (strncasestr_check(hline + 17, "Basic")) {
                    h->auth_avail |= CURLAUTH_BASIC;
                } else if (strncasestr_check(hline + 17, "Digest")) {
                    h->auth_avail |= CURLAUTH_DIGEST;
                } else {
                    h->auth_avail |= CURLAUTH_BASIC;
                }
            }
            free(hline);

            pos = line_end;
        }

        if (status_code == 401 && h->auth_avail == 0) {
            h->auth_avail = CURLAUTH_BASIC;
        }

        /* Handle HTTP redirects if enabled */
        if (h->follow_location && location_hdr &&
            (status_code == 301 || status_code == 302 || status_code == 303 ||
             status_code == 307 || status_code == 308)) {
            if (redirects >= max_redirs) {
                free(location_hdr);
                free_parsed_url(&purl);
                bb_free(&raw_resp);
                set_err(h, "Maximum (%ld) redirects followed", max_redirs);
                return CURLE_TOO_MANY_REDIRECTS;
            }
            char *next_url = resolve_redirect_url(&purl, location_hdr);
            free(location_hdr);
            free_parsed_url(&purl);
            bb_free(&raw_resp);
            if (!next_url) return CURLE_OUT_OF_MEMORY;
            free(h->effective_url);
            h->effective_url = next_url;
            if (status_code == 303) {
                h->post = 0;
                h->postfields = NULL;
                h->httpget = 1;
            }
            redirects++;
            continue;
        }
        free(location_hdr);
        free_parsed_url(&purl);

        /* Decode response body */
        const unsigned char *raw_body = raw_resp.data + body_offset;
        size_t raw_body_len = (body_offset <= raw_resp.len) ? (raw_resp.len - body_offset) : 0;

        byte_buf_t unchunked = { NULL, 0, 0 };
        const unsigned char *decoded_body = raw_body;
        size_t decoded_len = raw_body_len;

        if (resp_chunked && raw_body_len > 0) {
            size_t cpos = 0;
            while (cpos < raw_body_len) {
                /* Parse hex chunk size */
                while (cpos < raw_body_len && (raw_body[cpos] == '\r' || raw_body[cpos] == '\n'))
                    cpos++;
                if (cpos >= raw_body_len) break;
                char hexbuf[32];
                size_t hidx = 0;
                while (cpos < raw_body_len && raw_body[cpos] != '\r' && raw_body[cpos] != '\n' && raw_body[cpos] != ';') {
                    if (hidx + 1 < sizeof(hexbuf)) hexbuf[hidx++] = (char)raw_body[cpos];
                    cpos++;
                }
                hexbuf[hidx] = '\0';
                while (cpos < raw_body_len && raw_body[cpos] != '\n') cpos++;
                if (cpos < raw_body_len && raw_body[cpos] == '\n') cpos++;
                if (hidx == 0) break;
                size_t csize = (size_t)strtoul(hexbuf, NULL, 16);
                if (csize == 0) break;
                if (cpos + csize > raw_body_len) {
                    csize = raw_body_len - cpos;
                }
                bb_append(&unchunked, raw_body + cpos, csize);
                cpos += csize;
                if (cpos < raw_body_len && raw_body[cpos] == '\r') cpos++;
                if (cpos < raw_body_len && raw_body[cpos] == '\n') cpos++;
            }
            decoded_body = unchunked.data ? unchunked.data : (const unsigned char *)"";
            decoded_len = unchunked.len;
        }

        byte_buf_t gunzipped = { NULL, 0, 0 };
        if (resp_gzip && decoded_len > 0) {
            if (decompress_gzip_buf(decoded_body, decoded_len, &gunzipped) == 0) {
                decoded_body = gunzipped.data ? gunzipped.data : (const unsigned char *)"";
                decoded_len = gunzipped.len;
            }
        }

        if (h->failonerror && status_code >= 400) {
            set_err(h, "The requested URL returned error: %ld", status_code);
            bb_free(&gunzipped);
            bb_free(&unchunked);
            bb_free(&raw_resp);
            return CURLE_HTTP_RETURNED_ERROR;
        }

        if (!h->nobody && decoded_len > 0) {
            curl_write_callback wfn = h->write_fn ? h->write_fn : default_write_cb;
            size_t written = wfn((char *)decoded_body, 1, decoded_len, h->write_data);
            if (written != decoded_len) {
                bb_free(&gunzipped);
                bb_free(&unchunked);
                bb_free(&raw_resp);
                return CURLE_WRITE_ERROR;
            }
        }

        bb_free(&gunzipped);
        bb_free(&unchunked);
        bb_free(&raw_resp);
        return CURLE_OK;
    }
}

/* --------------------------------------------------------------------------
 * Global, Easy, Slist, and Multi API Functions
 * -------------------------------------------------------------------------- */
void curl_free(void *p) {
    free(p);
}

CURLcode curl_global_init(long flags) {
    (void)flags;
    return CURLE_OK;
}

void curl_global_cleanup(void) {}

CURLsslset curl_global_sslset(int id, const char *name, const curl_ssl_backend ***avail) {
    (void)id;
    (void)name;
    static const curl_ssl_backend bearssl_backend = { 1, "bearssl" };
    static const curl_ssl_backend *backends[] = { &bearssl_backend, NULL };
    if (avail) *avail = backends;
    return CURLSSLSET_OK;
}

curl_version_info_data *curl_version_info(int age) {
    (void)age;
    static const char * const protos[] = { "http", "https", NULL };
    static curl_version_info_data info = {
        CURLVERSION_NOW,
        LIBCURL_VERSION,
        LIBCURL_VERSION_NUM,
        "i386-pc-ipo_os",
        0,
        "BearSSL/0.6",
        0,
        "1.3.1",
        protos
    };
    return &info;
}

CURL *curl_easy_init(void) {
    ipo_curl_easy_t *h = (ipo_curl_easy_t *)calloc(1, sizeof(ipo_curl_easy_t));
    if (!h) return NULL;
    h->postfieldsize = -1L;
    h->postfieldsize_large = -1;
    h->maxredirs = 20L;
    h->httpauth = (long)CURLAUTH_ANY;
    h->ssl_verifypeer = 1L;
    h->ssl_verifyhost = 2L;
    return (CURL *)h;
}

void curl_easy_cleanup(CURL *curl) {
    ipo_curl_easy_t *h = (ipo_curl_easy_t *)curl;
    if (!h) return;
    free(h->url);
    free(h->effective_url);
    free(h->useragent);
    free(h->custom_request);
    free(h->username);
    free(h->password);
    free(h->userpwd);
    free(h->range);
    free(h->accept_encoding);
    free(h->content_type);
    free(h->cainfo);
    free(h->capath);
    free(h);
}

static char *dup_or_null(const char *s) {
    return s ? strdup(s) : NULL;
}

CURL *curl_easy_duphandle(CURL *curl) {
    ipo_curl_easy_t *src = (ipo_curl_easy_t *)curl;
    if (!src) return NULL;
    ipo_curl_easy_t *dst = (ipo_curl_easy_t *)calloc(1, sizeof(ipo_curl_easy_t));
    if (!dst) return NULL;
    *dst = *src;
    dst->url = dup_or_null(src->url);
    dst->effective_url = dup_or_null(src->effective_url);
    dst->useragent = dup_or_null(src->useragent);
    dst->custom_request = dup_or_null(src->custom_request);
    dst->username = dup_or_null(src->username);
    dst->password = dup_or_null(src->password);
    dst->userpwd = dup_or_null(src->userpwd);
    dst->range = dup_or_null(src->range);
    dst->accept_encoding = dup_or_null(src->accept_encoding);
    dst->content_type = dup_or_null(src->content_type);
    dst->cainfo = dup_or_null(src->cainfo);
    dst->capath = dup_or_null(src->capath);
    return (CURL *)dst;
}

static void replace_str(char **dst, const char *src) {
    free(*dst);
    *dst = src ? strdup(src) : NULL;
}

CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...) {
    ipo_curl_easy_t *h = (ipo_curl_easy_t *)curl;
    if (!h) return CURLE_FAILED_INIT;

    va_list ap;
    va_start(ap, option);

    switch (option) {
    case CURLOPT_URL:
        replace_str(&h->url, va_arg(ap, const char *));
        break;
    case CURLOPT_USERAGENT:
        replace_str(&h->useragent, va_arg(ap, const char *));
        break;
    case CURLOPT_CUSTOMREQUEST:
        replace_str(&h->custom_request, va_arg(ap, const char *));
        break;
    case CURLOPT_USERNAME:
        replace_str(&h->username, va_arg(ap, const char *));
        break;
    case CURLOPT_PASSWORD:
        replace_str(&h->password, va_arg(ap, const char *));
        break;
    case CURLOPT_USERPWD:
        replace_str(&h->userpwd, va_arg(ap, const char *));
        break;
    case CURLOPT_RANGE:
        replace_str(&h->range, va_arg(ap, const char *));
        break;
    case CURLOPT_ENCODING:
        replace_str(&h->accept_encoding, va_arg(ap, const char *));
        break;
    case CURLOPT_ERRORBUFFER:
        h->errorbuf = va_arg(ap, char *);
        break;
    case CURLOPT_POSTFIELDS:
        h->postfields = va_arg(ap, const void *);
        if (h->postfields) h->post = 1L;
        break;
    case CURLOPT_POSTFIELDSIZE:
        h->postfieldsize = va_arg(ap, long);
        break;
    case CURLOPT_POSTFIELDSIZE_LARGE:
        h->postfieldsize_large = va_arg(ap, curl_off_t);
        break;
    case CURLOPT_POST:
        h->post = va_arg(ap, long);
        if (h->post) {
            h->nobody = 0L;
            h->httpget = 0L;
        }
        break;
    case CURLOPT_NOBODY:
        h->nobody = va_arg(ap, long);
        if (h->nobody) {
            h->post = 0L;
            h->httpget = 0L;
        }
        break;
    case CURLOPT_HTTPGET:
        h->httpget = va_arg(ap, long);
        if (h->httpget) {
            h->post = 0L;
            h->nobody = 0L;
            h->upload = 0L;
            h->postfields = NULL;
        }
        break;
    case CURLOPT_UPLOAD:
    case CURLOPT_PUT:
        h->upload = va_arg(ap, long);
        break;
    case CURLOPT_FAILONERROR:
        h->failonerror = va_arg(ap, long);
        break;
    case CURLOPT_FOLLOWLOCATION:
        h->follow_location = va_arg(ap, long);
        break;
    case CURLOPT_MAXREDIRS:
        h->maxredirs = va_arg(ap, long);
        break;
    case CURLOPT_HTTPAUTH:
        h->httpauth = va_arg(ap, long);
        break;
    case CURLOPT_VERBOSE:
        h->verbose = va_arg(ap, long);
        break;
    case CURLOPT_HTTPHEADER:
        h->headers = va_arg(ap, struct curl_slist *);
        break;
    case CURLOPT_WRITEFUNCTION:
        h->write_fn = va_arg(ap, curl_write_callback);
        break;
    case CURLOPT_WRITEDATA:
        h->write_data = va_arg(ap, void *);
        break;
    case CURLOPT_HEADERFUNCTION:
        h->header_fn = va_arg(ap, curl_write_callback);
        break;
    case CURLOPT_HEADERDATA:
        h->header_data = va_arg(ap, void *);
        break;
    case CURLOPT_READFUNCTION:
        h->read_fn = va_arg(ap, curl_read_callback);
        break;
    case CURLOPT_INFILE:
        h->infile = va_arg(ap, void *);
        break;
    case CURLOPT_SEEKFUNCTION:
        h->seek_fn = va_arg(ap, curl_seek_callback);
        break;
    case CURLOPT_SEEKDATA:
        h->seek_data = va_arg(ap, void *);
        break;
    case CURLOPT_DEBUGFUNCTION:
        h->debug_fn = va_arg(ap, curl_debug_callback);
        break;
    case CURLOPT_DEBUGDATA:
        h->debug_data = va_arg(ap, void *);
        break;
    case CURLOPT_PRIVATE:
        h->private_data = va_arg(ap, void *);
        break;
    case CURLOPT_SSL_VERIFYPEER:
        h->ssl_verifypeer = va_arg(ap, long);
        break;
    case CURLOPT_SSL_VERIFYHOST:
        h->ssl_verifyhost = va_arg(ap, long);
        break;
    case CURLOPT_CAINFO:
        replace_str(&h->cainfo, va_arg(ap, const char *));
        break;
    case CURLOPT_CAPATH:
        replace_str(&h->capath, va_arg(ap, const char *));
        break;
    default:
        (void)va_arg(ap, void *);
        break;
    }

    va_end(ap);
    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...) {
    ipo_curl_easy_t *h = (ipo_curl_easy_t *)curl;
    if (!h) return CURLE_FAILED_INIT;

    va_list ap;
    va_start(ap, info);

    switch (info) {
    case CURLINFO_RESPONSE_CODE: {
        long *out = va_arg(ap, long *);
        if (out) *out = h->response_code;
        break;
    }
    case CURLINFO_HTTP_CONNECTCODE: {
        long *out = va_arg(ap, long *);
        if (out) *out = h->connect_code;
        break;
    }
    case CURLINFO_HTTPAUTH_AVAIL: {
        long *out = va_arg(ap, long *);
        if (out) *out = h->auth_avail;
        break;
    }
    case CURLINFO_EFFECTIVE_URL: {
        const char **out = va_arg(ap, const char **);
        if (out) *out = h->effective_url ? h->effective_url : (h->url ? h->url : "");
        break;
    }
    case CURLINFO_CONTENT_TYPE: {
        const char **out = va_arg(ap, const char **);
        if (out) *out = h->content_type;
        break;
    }
    case CURLINFO_PRIVATE: {
        void **out = va_arg(ap, void **);
        if (out) *out = h->private_data;
        break;
    }
    default:
        break;
    }

    va_end(ap);
    return CURLE_OK;
}

const char *curl_easy_strerror(CURLcode code) {
    switch (code) {
    case CURLE_OK: return "No error";
    case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
    case CURLE_URL_MALFORMAT: return "URL using bad/illegal format or missing URL";
    case CURLE_COULDNT_RESOLVE_HOST: return "Couldn't resolve host name";
    case CURLE_COULDNT_CONNECT: return "Couldn't connect to server";
    case CURLE_HTTP_RETURNED_ERROR: return "Server returned an HTTP error";
    case CURLE_WRITE_ERROR: return "Failed writing received data";
    case CURLE_READ_ERROR: return "Failed reading input data";
    case CURLE_OUT_OF_MEMORY: return "Out of memory";
    case CURLE_OPERATION_TIMEDOUT: return "Operation timed out";
    case CURLE_TOO_MANY_REDIRECTS: return "Number of redirects hit maximum amount";
    case CURLE_GOT_NOTHING: return "Server returned nothing (no headers, no data)";
    case CURLE_SEND_ERROR: return "Failed sending data to the peer";
    case CURLE_RECV_ERROR: return "Failure when receiving data from the peer";
    default: return "cURL transfer error";
    }
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data) {
    if (!data) return list;
    struct curl_slist *node = (struct curl_slist *)calloc(1, sizeof(struct curl_slist));
    if (!node) return NULL;
    node->data = strdup(data);
    if (!node->data) {
        free(node);
        return NULL;
    }
    if (!list) return node;
    struct curl_slist *cur = list;
    while (cur->next) cur = cur->next;
    cur->next = node;
    return list;
}

void curl_slist_free_all(struct curl_slist *list) {
    while (list) {
        struct curl_slist *next = list->next;
        free(list->data);
        free(list);
        list = next;
    }
}

CURLM *curl_multi_init(void) {
    ipo_curl_multi_t *m = (ipo_curl_multi_t *)calloc(1, sizeof(ipo_curl_multi_t));
    return (CURLM *)m;
}

CURLMcode curl_multi_cleanup(CURLM *multi_handle) {
    ipo_curl_multi_t *m = (ipo_curl_multi_t *)multi_handle;
    if (m) {
        free(m->handles);
        free(m->states);
        free(m->results);
        free(m);
    }
    return CURLM_OK;
}

CURLMcode curl_multi_add_handle(CURLM *multi_handle, CURL *curl_handle) {
    ipo_curl_multi_t *m = (ipo_curl_multi_t *)multi_handle;
    if (!m || !curl_handle) return CURLM_BAD_HANDLE;
    int slot = -1;
    for (int i = 0; i < m->cap; i++) {
        if (m->states[i] == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        int ncap = m->cap > 0 ? (m->cap * 2) : 16;
        if (ncap <= m->cap || (size_t)ncap > SIZE_MAX / sizeof(ipo_curl_easy_t *)) {
            return CURLM_OUT_OF_MEMORY;
        }
        ipo_curl_easy_t **nh = (ipo_curl_easy_t **)realloc(m->handles, (size_t)ncap * sizeof(ipo_curl_easy_t *));
        if (!nh) return CURLM_OUT_OF_MEMORY;
        m->handles = nh;
        int *ns = (int *)realloc(m->states, (size_t)ncap * sizeof(int));
        if (!ns) return CURLM_OUT_OF_MEMORY;
        m->states = ns;
        CURLcode *nr = (CURLcode *)realloc(m->results, (size_t)ncap * sizeof(CURLcode));
        if (!nr) return CURLM_OUT_OF_MEMORY;
        m->results = nr;
        for (int i = m->cap; i < ncap; i++) {
            m->handles[i] = NULL;
            m->states[i] = 0;
            m->results[i] = CURLE_OK;
        }
        slot = m->cap;
        m->cap = ncap;
    }
    m->handles[slot] = (ipo_curl_easy_t *)curl_handle;
    m->states[slot] = 1; /* pending */
    m->results[slot] = CURLE_OK;
    return CURLM_OK;
}

CURLMcode curl_multi_remove_handle(CURLM *multi_handle, CURL *curl_handle) {
    ipo_curl_multi_t *m = (ipo_curl_multi_t *)multi_handle;
    if (!m || !curl_handle) return CURLM_BAD_HANDLE;
    for (int i = 0; i < m->cap; i++) {
        if (m->handles[i] == (ipo_curl_easy_t *)curl_handle && m->states[i] != 0) {
            m->handles[i] = NULL;
            m->states[i] = 0;
            m->results[i] = CURLE_OK;
            break;
        }
    }
    return CURLM_OK;
}

CURLMcode curl_multi_perform(CURLM *multi_handle, int *running_handles) {
    ipo_curl_multi_t *m = (ipo_curl_multi_t *)multi_handle;
    if (!m) return CURLM_BAD_HANDLE;
    for (int i = 0; i < m->cap; i++) {
        if (m->states[i] == 1 && m->handles[i]) {
            m->results[i] = curl_easy_perform((CURL *)m->handles[i]);
            m->states[i] = 2; /* done, message queued */
        }
    }
    if (running_handles) *running_handles = 0;
    return CURLM_OK;
}

CURLMcode curl_multi_fdset(CURLM *multi_handle, fd_set *read_fd_set,
                           fd_set *write_fd_set, fd_set *exc_fd_set, int *max_fd) {
    (void)multi_handle;
    (void)read_fd_set;
    (void)write_fd_set;
    (void)exc_fd_set;
    if (max_fd) *max_fd = -1;
    return CURLM_OK;
}

CURLMcode curl_multi_timeout(CURLM *multi_handle, long *milliseconds) {
    (void)multi_handle;
    if (milliseconds) *milliseconds = 0;
    return CURLM_OK;
}

CURLMsg *curl_multi_info_read(CURLM *multi_handle, int *msgs_in_queue) {
    ipo_curl_multi_t *m = (ipo_curl_multi_t *)multi_handle;
    if (!m) {
        if (msgs_in_queue) *msgs_in_queue = 0;
        return NULL;
    }
    int found = -1;
    int remaining = 0;
    for (int i = 0; i < m->cap; i++) {
        if (m->states[i] == 2) {
            if (found < 0) found = i;
            else remaining++;
        }
    }
    if (msgs_in_queue) *msgs_in_queue = remaining;
    if (found < 0) return NULL;

    m->states[found] = 3; /* done, message read */
    m->last_msg.msg = CURLMSG_DONE;
    m->last_msg.easy_handle = (CURL *)m->handles[found];
    m->last_msg.data.result = m->results[found];
    return &m->last_msg;
}

const char *curl_multi_strerror(CURLMcode code) {
    switch (code) {
    case CURLM_OK: return "No error";
    case CURLM_BAD_HANDLE: return "Invalid multi handle";
    case CURLM_BAD_EASY_HANDLE: return "Invalid easy handle";
    case CURLM_OUT_OF_MEMORY: return "Out of memory";
    default: return "Multi error";
    }
}
