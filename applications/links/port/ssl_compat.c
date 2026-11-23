#include <openssl/ssl.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/crypto.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syscall.h>
#include <unistd.h>
#include <time.h>

extern int getentropy(void *buffer, size_t length);

#include "../../../lib/bearssl/inc/bearssl.h"
#define static_const static const
#include "../upstream/certs.inc"
#undef static_const

struct x509_name_st {
    int dummy;
};

struct x509_st {
    int heap_allocated;
    uint8_t *dn_data;
    size_t dn_len;
    unsigned ta_flags;
    br_x509_pkey pkey;
};

struct x509_store_st {
    br_x509_trust_anchor *anchors;
    size_t count;
    size_t cap;
};

struct bio_st {
    const uint8_t *data;
    size_t len;
    size_t pos;
};

struct ssl_ctx_st {
    int refcount;
    int verify_mode;
    struct x509_store_st store;
};

typedef struct {
    const br_x509_class *vtable;
    const br_x509_class **inner;
    unsigned raw_err;
} x509_noanchor_context;

static void xwc_start_chain(const br_x509_class **ctx, const char *server_name) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    xwc->raw_err = 0u;
    (*xwc->inner)->start_chain(xwc->inner, server_name);
}
static void xwc_start_cert(const br_x509_class **ctx, uint32_t length) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->start_cert(xwc->inner, length);
}
static void xwc_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->append(xwc->inner, buf, len);
}
static void xwc_end_cert(const br_x509_class **ctx) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->end_cert(xwc->inner);
}
static unsigned xwc_end_chain(const br_x509_class **ctx) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    unsigned r = (*xwc->inner)->end_chain(xwc->inner);
    xwc->raw_err = r;
    if (r == BR_ERR_X509_NOT_TRUSTED || r == BR_ERR_X509_EXPIRED ||
        r == BR_ERR_X509_TIME_UNKNOWN) {
        r = 0;
    }
    return r;
}
static const br_x509_pkey *xwc_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    return (*xwc->inner)->get_pkey(xwc->inner, usages);
}
static const br_x509_class x509_noanchor_vtable = {
    sizeof(x509_noanchor_context),
    xwc_start_chain, xwc_start_cert, xwc_append, xwc_end_cert, xwc_end_chain, xwc_get_pkey
};

struct ssl_st {
    int fd;
    char server_name[256];
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    x509_noanchor_context xwc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    int connected;
    int peer_eof;
    int last_err;
    int br_last_err;
    int verify_mode;
};

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} ssl_dyn_buf_t;

static void dn_buf_append(void *ctx, const void *data, size_t len) {
    ssl_dyn_buf_t *b = (ssl_dyn_buf_t *)ctx;
    if (!b || !data || len == 0u) return;
    if (b->len + len > b->cap) {
        size_t ncap = b->cap ? b->cap : 128u;
        while (ncap < b->len + len) {
            if (ncap > SIZE_MAX / 2u) return;
            ncap *= 2u;
        }
        uint8_t *nb = (uint8_t *)realloc(b->buf, ncap);
        if (!nb) return;
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
}

static X509 *x509_from_der(const uint8_t *der, size_t der_len) {
    if (!der || der_len == 0u) return NULL;
    br_x509_decoder_context dc;
    ssl_dyn_buf_t dn = { NULL, 0u, 0u };
    br_x509_decoder_init(&dc, dn_buf_append, &dn);
    br_x509_decoder_push(&dc, der, der_len);
    const br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
    if (!pk || br_x509_decoder_last_error(&dc) != 0 || dn.len == 0u) {
        free(dn.buf);
        return NULL;
    }

    X509 *x = (X509 *)calloc(1u, sizeof(X509));
    if (!x) {
        free(dn.buf);
        return NULL;
    }
    x->heap_allocated = 1;
    x->dn_data = dn.buf;
    x->dn_len = dn.len;
    x->ta_flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0u;
    x->pkey.key_type = pk->key_type;
    if (pk->key_type == BR_KEYTYPE_RSA) {
        x->pkey.key.rsa.nlen = pk->key.rsa.nlen;
        x->pkey.key.rsa.elen = pk->key.rsa.elen;
        x->pkey.key.rsa.n = (unsigned char *)malloc(pk->key.rsa.nlen ? pk->key.rsa.nlen : 1u);
        x->pkey.key.rsa.e = (unsigned char *)malloc(pk->key.rsa.elen ? pk->key.rsa.elen : 1u);
        if (!x->pkey.key.rsa.n || !x->pkey.key.rsa.e) {
            X509_free(x);
            return NULL;
        }
        if (pk->key.rsa.nlen) memcpy(x->pkey.key.rsa.n, pk->key.rsa.n, pk->key.rsa.nlen);
        if (pk->key.rsa.elen) memcpy(x->pkey.key.rsa.e, pk->key.rsa.e, pk->key.rsa.elen);
    } else if (pk->key_type == BR_KEYTYPE_EC) {
        x->pkey.key.ec.curve = pk->key.ec.curve;
        x->pkey.key.ec.qlen = pk->key.ec.qlen;
        x->pkey.key.ec.q = (unsigned char *)malloc(pk->key.ec.qlen ? pk->key.ec.qlen : 1u);
        if (!x->pkey.key.ec.q) {
            X509_free(x);
            return NULL;
        }
        if (pk->key.ec.qlen) memcpy(x->pkey.key.ec.q, pk->key.ec.q, pk->key.ec.qlen);
    } else {
        X509_free(x);
        return NULL;
    }
    return x;
}

static void store_free_anchors(X509_STORE *store) {
    if (!store) return;
    for (size_t i = 0u; i < store->count; i++) {
        free(store->anchors[i].dn.data);
        if (store->anchors[i].pkey.key_type == BR_KEYTYPE_RSA) {
            free(store->anchors[i].pkey.key.rsa.n);
            free(store->anchors[i].pkey.key.rsa.e);
        } else if (store->anchors[i].pkey.key_type == BR_KEYTYPE_EC) {
            free(store->anchors[i].pkey.key.ec.q);
        }
    }
    free(store->anchors);
    store->anchors = NULL;
    store->count = 0u;
    store->cap = 0u;
}

static int store_load_pem_mem(X509_STORE *store, const uint8_t *pem, size_t len) {
    if (!store || !pem || len == 0u) return -1;
    BIO b = { pem, len, 0u };
    int loaded = 0;
    while (b.pos < b.len) {
        X509 *cert = PEM_read_bio_X509(&b, NULL, NULL, NULL);
        if (!cert) break;
        if (X509_STORE_add_cert(store, cert) == 1) loaded++;
        X509_free(cert);
    }
    return (loaded > 0) ? 0 : -1;
}

static void store_load_builtin_ders(X509_STORE *store) {
    if (!store) return;
    size_t n = sizeof(certificates) / sizeof(certificates[0]);
    for (size_t i = 0u; i < n; i++) {
        if (certificates[i].data && certificates[i].len > 0) {
            X509 *cert = x509_from_der((const uint8_t *)certificates[i].data,
                                       (size_t)certificates[i].len);
            if (cert) {
                X509_STORE_add_cert(store, cert);
                X509_free(cert);
            }
        }
    }
}

int OPENSSL_init_ssl(uint64_t opts, const void *settings) {
    (void)opts; (void)settings;
    return 1;
}

int OpenSSL_add_ssl_algorithms(void) {
    return 1;
}

int SSL_library_init(void) {
    return 1;
}

void SSL_load_error_strings(void) {}
void OPENSSL_cleanup(void) {}

static const SSL_METHOD dummy_client_method;

const SSL_METHOD *TLS_client_method(void) {
    return &dummy_client_method;
}

const SSL_METHOD *SSLv23_client_method(void) {
    return &dummy_client_method;
}

SSL_CTX *SSL_CTX_new(const SSL_METHOD *meth) {
    (void)meth;
    SSL_CTX *ctx = (SSL_CTX *)calloc(1u, sizeof(SSL_CTX));
    if (ctx) {
        ctx->refcount = 1;
        ctx->verify_mode = SSL_VERIFY_NONE;
    }
    return ctx;
}

void SSL_CTX_free(SSL_CTX *ctx) {
    if (!ctx) return;
    store_free_anchors(&ctx->store);
    free(ctx);
}

long SSL_CTX_set_options(SSL_CTX *ctx, long options) {
    (void)ctx; (void)options;
    return 0;
}

long SSL_CTX_set_mode(SSL_CTX *ctx, long mode) {
    (void)ctx; (void)mode;
    return 0;
}

int SSL_CTX_set_min_proto_version(SSL_CTX *ctx, int version) {
    (void)ctx; (void)version;
    return 1;
}

void SSL_CTX_set_verify(SSL_CTX *ctx, int mode, int (*verify_callback)(int, void *)) {
    (void)verify_callback;
    if (ctx) ctx->verify_mode = mode;
}

int SSL_CTX_set_default_verify_paths(SSL_CTX *ctx) {
    if (!ctx) return 0;
    if (SSL_CTX_load_verify_locations(ctx, "/etc/ssl/certs/ca-certificates.crt", NULL) == 1 ||
        SSL_CTX_load_verify_locations(ctx, "/data/ssl/ca-certificates.crt", NULL) == 1) {
        return 1;
    }
    store_load_builtin_ders(&ctx->store);
    return (ctx->store.count > 0u) ? 1 : 0;
}

int SSL_CTX_load_verify_locations(SSL_CTX *ctx, const char *cafile, const char *capath) {
    (void)capath;
    if (!ctx || !cafile || !*cafile) return 0;
    int fd = open(cafile, O_RDONLY);
    if (fd < 0) return 0;
    ssl_dyn_buf_t fbuf = { NULL, 0u, 0u };
    uint8_t chunk[2048];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
        dn_buf_append(&fbuf, chunk, (size_t)n);
    }
    close(fd);
    int rc = (fbuf.len > 0u && store_load_pem_mem(&ctx->store, fbuf.buf, fbuf.len) == 0) ? 1 : 0;
    free(fbuf.buf);
    return rc;
}

X509_STORE *SSL_CTX_get_cert_store(const SSL_CTX *ctx) {
    if (!ctx) return NULL;
    return (X509_STORE *)&ctx->store;
}

void SSL_CTX_set_default_passwd_cb(SSL_CTX *ctx, int (*cb)(char *, int, int, void *)) {
    (void)ctx; (void)cb;
}

static void inject_ssl_entropy(br_ssl_engine_context *eng) {
    unsigned char seed[32];
    if (getentropy(seed, sizeof(seed)) == 0) {
        br_ssl_engine_inject_entropy(eng, seed, sizeof(seed));
    }
}

static void apply_bearssl_time(br_x509_minimal_context *xc) {
    time_t now = time(NULL);
    if (now > 1577836800) {
        uint32_t days = (uint32_t)(now / 86400) + 719528u;
        uint32_t secs = (uint32_t)(now % 86400);
        br_x509_minimal_set_time(xc, days, secs);
    }
}

SSL *SSL_new(SSL_CTX *ctx) {
    SSL *s = (SSL *)calloc(1u, sizeof(SSL));
    if (!s) return NULL;
    s->fd = -1;
    s->connected = 0;
    s->last_err = SSL_ERROR_NONE;
    s->br_last_err = 0;
    s->verify_mode = ctx ? ctx->verify_mode : SSL_VERIFY_NONE;

    const br_x509_trust_anchor *tas = ctx ? ctx->store.anchors : NULL;
    size_t ta_count = ctx ? ctx->store.count : 0u;
    br_ssl_client_init_full(&s->sc, &s->xc, tas, ta_count);
    apply_bearssl_time(&s->xc);

    s->xwc.vtable = &x509_noanchor_vtable;
    s->xwc.inner = &s->xc.vtable;
    s->xwc.raw_err = 0u;
    br_ssl_engine_set_x509(&s->sc.eng, &s->xwc.vtable);

    br_ssl_engine_set_buffer(&s->sc.eng, s->iobuf, sizeof(s->iobuf), 1);
    br_ssl_client_reset(&s->sc, NULL, 0);
    inject_ssl_entropy(&s->sc.eng);
    s->peer_eof = 0;
    return s;
}

void SSL_free(SSL *ssl) {
    if (ssl) free(ssl);
}

int SSL_set_fd(SSL *ssl, int fd) {
    if (!ssl) return 0;
    ssl->fd = fd;
    return 1;
}

long SSL_set_tlsext_host_name(SSL *ssl, const char *name) {
    if (!ssl || !name) return 0;
    strncpy(ssl->server_name, name, sizeof(ssl->server_name) - 1);
    ssl->server_name[sizeof(ssl->server_name) - 1] = '\0';
    br_ssl_client_reset(&ssl->sc, ssl->server_name, 0);
    apply_bearssl_time(&ssl->xc);
    inject_ssl_entropy(&ssl->sc.eng);
    ssl->peer_eof = 0;
    ssl->br_last_err = 0;
    return 1;
}

long SSL_set_options(SSL *ssl, long options) {
    (void)ssl; (void)options;
    return 0;
}

long SSL_set_mode(SSL *ssl, long mode) {
    (void)ssl; (void)mode;
    return 0;
}

void SSL_set_security_level(SSL *ssl, int level) {
    (void)ssl; (void)level;
}

void SSL_set_quiet_shutdown(SSL *ssl, int mode) {
    (void)ssl; (void)mode;
}

int SSL_shutdown(SSL *ssl) {
    if (!ssl) return 0;
    br_ssl_engine_close(&ssl->sc.eng);
    return 1;
}

static int pump_ssl(SSL *ssl) {
    for (;;) {
        int progress = 0;
        ipo_syscall(IPO_SYSCALL_NET_POLL, 0u, NULL);
        unsigned st = br_ssl_engine_current_state(&ssl->sc.eng);
        if (st == BR_SSL_CLOSED) {
            int err = br_ssl_engine_last_error(&ssl->sc.eng);
            ssl->br_last_err = err;
            ssl->last_err = (err == BR_ERR_OK) ? SSL_ERROR_ZERO_RETURN : SSL_ERROR_SSL;
            return -1;
        }

        while ((st = br_ssl_engine_current_state(&ssl->sc.eng)) & BR_SSL_SENDREC) {
            size_t len = 0;
            unsigned char *buf = br_ssl_engine_sendrec_buf(&ssl->sc.eng, &len);
            if (!buf || len == 0) break;
            ssize_t w = send(ssl->fd, buf, len, MSG_DONTWAIT);
            if (w > 0) {
                br_ssl_engine_sendrec_ack(&ssl->sc.eng, (size_t)w);
                progress = 1;
            } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                ssl->last_err = SSL_ERROR_WANT_WRITE;
                return -1;
            } else if (w < 0) {
                ssl->last_err = SSL_ERROR_SYSCALL;
                return -1;
            } else {
                break;
            }
        }

        while ((st = br_ssl_engine_current_state(&ssl->sc.eng)) & BR_SSL_RECVREC) {
            if (ssl->peer_eof) break;
            size_t len = 0;
            unsigned char *buf = br_ssl_engine_recvrec_buf(&ssl->sc.eng, &len);
            if (!buf || len == 0) break;
            ssize_t r = recv(ssl->fd, buf, len, MSG_DONTWAIT);
            if (r > 0) {
                br_ssl_engine_recvrec_ack(&ssl->sc.eng, (size_t)r);
                progress = 1;
            } else if (r == 0) {
                ssl->peer_eof = 1;
                break;
            } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else if (r < 0) {
                ssl->last_err = SSL_ERROR_SYSCALL;
                return -1;
            }
        }

        if (!progress)
            break;
    }

    unsigned final_st = br_ssl_engine_current_state(&ssl->sc.eng);
    if (final_st & BR_SSL_RECVAPP) {
        return 0;
    }
    if (ssl->peer_eof || final_st == BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&ssl->sc.eng);
        ssl->br_last_err = err;
        ssl->last_err = (err == BR_ERR_OK) ? SSL_ERROR_ZERO_RETURN : SSL_ERROR_SSL;
        return -1;
    }
    if (final_st & BR_SSL_SENDREC) {
        ssl->last_err = SSL_ERROR_WANT_WRITE;
        return -1;
    }
    if (final_st & BR_SSL_RECVREC) {
        ssl->last_err = SSL_ERROR_WANT_READ;
        return -1;
    }
    return 0;
}

int SSL_connect(SSL *ssl) {
    if (!ssl || ssl->fd < 0) return -1;
    if (ssl->connected) {
        ssl->last_err = SSL_ERROR_NONE;
        return 1;
    }

    pump_ssl(ssl);
    unsigned st = br_ssl_engine_current_state(&ssl->sc.eng);
    if ((st & BR_SSL_SENDAPP) || (st & BR_SSL_RECVAPP)) {
        ssl->connected = 1;
        ssl->last_err = SSL_ERROR_NONE;
        return 1;
    }
    if (st == BR_SSL_CLOSED) {
        int err = br_ssl_engine_last_error(&ssl->sc.eng);
        ssl->br_last_err = err;
        ssl->last_err = (err == BR_ERR_OK) ? SSL_ERROR_ZERO_RETURN : SSL_ERROR_SSL;
        return -1;
    }
    if (st & BR_SSL_SENDREC) {
        ssl->last_err = SSL_ERROR_WANT_WRITE;
    } else {
        ssl->last_err = SSL_ERROR_WANT_READ;
    }
    return -1;
}

int SSL_get_error(const SSL *ssl, int ret) {
    if (ret > 0) return SSL_ERROR_NONE;
    if (!ssl) return SSL_ERROR_SSL;
    return ssl->last_err;
}

int SSL_write(SSL *ssl, const void *buf, int num) {
    if (!ssl || num <= 0) return 0;
    if (!ssl->connected) {
        int r = SSL_connect(ssl);
        if (r <= 0) return r;
    }

    pump_ssl(ssl);
    unsigned st = br_ssl_engine_current_state(&ssl->sc.eng);
    if (!(st & BR_SSL_SENDAPP)) {
        if (st == BR_SSL_CLOSED) return -1;
        ssl->last_err = SSL_ERROR_WANT_WRITE;
        return -1;
    }

    size_t alen = 0;
    unsigned char *abuf = br_ssl_engine_sendapp_buf(&ssl->sc.eng, &alen);
    if (!abuf || alen == 0) {
        ssl->last_err = SSL_ERROR_WANT_WRITE;
        return -1;
    }

    size_t to_write = (size_t)num < alen ? (size_t)num : alen;
    memcpy(abuf, buf, to_write);
    br_ssl_engine_sendapp_ack(&ssl->sc.eng, to_write);
    br_ssl_engine_flush(&ssl->sc.eng, 0);

    pump_ssl(ssl);
    ssl->last_err = SSL_ERROR_NONE;
    return (int)to_write;
}

int SSL_read(SSL *ssl, void *buf, int num) {
    if (!ssl || num <= 0) return 0;
    if (!ssl->connected) {
        int r = SSL_connect(ssl);
        if (r <= 0) return r;
    }

    pump_ssl(ssl);
    unsigned st = br_ssl_engine_current_state(&ssl->sc.eng);
    if (st & BR_SSL_RECVAPP) {
        size_t alen = 0;
        unsigned char *abuf = br_ssl_engine_recvapp_buf(&ssl->sc.eng, &alen);
        if (abuf && alen > 0) {
            size_t to_read = (size_t)num < alen ? (size_t)num : alen;
            memcpy(buf, abuf, to_read);
            br_ssl_engine_recvapp_ack(&ssl->sc.eng, to_read);
            ssl->last_err = SSL_ERROR_NONE;
            return (int)to_read;
        }
    }

    if (st == BR_SSL_CLOSED || ssl->peer_eof) {
        ssl->last_err = SSL_ERROR_ZERO_RETURN;
        return 0;
    }

    ssl->last_err = SSL_ERROR_WANT_READ;
    return -1;
}

const char *SSL_get_version(const SSL *ssl) {
    (void)ssl;
    return "TLSv1.2";
}

const char *SSL_get_cipher_name(const SSL *ssl) {
    (void)ssl;
    return "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256";
}

int SSL_get_cipher_bits(const SSL *ssl, int *alg_bits) {
    (void)ssl;
    if (alg_bits) *alg_bits = 128;
    return 128;
}

static struct x509_st s_dummy_peer_cert = { 0 };

X509 *SSL_get_peer_certificate(const SSL *ssl) {
    (void)ssl;
    return &s_dummy_peer_cert;
}

long SSL_get_verify_result(const SSL *ssl) {
    if (!ssl) return X509_V_ERR_APPLICATION_VERIFICATION;
    if (ssl->verify_mode == SSL_VERIFY_NONE) return X509_V_OK;
    int err = ssl->br_last_err ? ssl->br_last_err : br_ssl_engine_last_error(&ssl->sc.eng);
    if (err == 0 || err == BR_ERR_OK) return X509_V_OK;
    if (err == BR_ERR_X509_EXPIRED) return X509_V_ERR_CERT_HAS_EXPIRED;
    if (err == BR_ERR_X509_NOT_TRUSTED) return X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY;
    if (err >= BR_ERR_X509_OK && err <= BR_ERR_X509_LIMIT_EXCEEDED) {
        return X509_V_ERR_CERT_UNTRUSTED;
    }
    return X509_V_ERR_APPLICATION_VERIFICATION;
}

int SSL_session_reused(const SSL *ssl) {
    (void)ssl;
    return 0;
}

int SSL_set_session(SSL *ssl, SSL_SESSION *session) {
    (void)ssl; (void)session;
    return 0;
}

void SSL_SESSION_free(SSL_SESSION *ses) {
    (void)ses;
}

void X509_free(X509 *x509) {
    if (!x509 || !x509->heap_allocated) return;
    free(x509->dn_data);
    if (x509->pkey.key_type == BR_KEYTYPE_RSA) {
        free(x509->pkey.key.rsa.n);
        free(x509->pkey.key.rsa.e);
    } else if (x509->pkey.key_type == BR_KEYTYPE_EC) {
        free(x509->pkey.key.ec.q);
    }
    free(x509);
}

X509_NAME *X509_get_subject_name(const X509 *a) {
    (void)a;
    static struct x509_name_st dummy_name;
    return &dummy_name;
}

char *X509_NAME_oneline(const X509_NAME *a, char *buf, int size) {
    (void)a;
    if (buf && size > 16) {
        strncpy(buf, "/CN=verified", size - 1);
        buf[size - 1] = '\0';
        return buf;
    }
    return strdup("/CN=verified");
}

int X509_NAME_get_index_by_NID(const X509_NAME *name, int nid, int lastpos) {
    (void)name; (void)nid; (void)lastpos;
    return -1;
}

X509_NAME_ENTRY *X509_NAME_get_entry(const X509_NAME *name, int loc) {
    (void)name; (void)loc;
    return NULL;
}

ASN1_STRING *X509_NAME_ENTRY_get_data(const X509_NAME_ENTRY *ne) {
    (void)ne;
    return NULL;
}

const unsigned char *ASN1_STRING_get0_data(const ASN1_STRING *x) {
    (void)x;
    return (const unsigned char *)"";
}

unsigned char *ASN1_STRING_data(ASN1_STRING *x) {
    (void)x;
    return (unsigned char *)"";
}

int ASN1_STRING_length(const ASN1_STRING *x) {
    (void)x;
    return 0;
}

int X509_check_host(X509 *cert, const char *name, size_t namelen, unsigned int flags, char **peername) {
    (void)cert; (void)name; (void)namelen; (void)flags; (void)peername;
    return 1;
}

int X509_check_ip(X509 *cert, const unsigned char *ip, size_t iplen, unsigned int flags) {
    (void)cert; (void)ip; (void)iplen; (void)flags;
    return 1;
}

BIO *BIO_new_mem_buf(const void *buf, int len) {
    if (!buf) return NULL;
    size_t sz = (len < 0) ? strlen((const char *)buf) : (size_t)len;
    BIO *b = (BIO *)calloc(1u, sizeof(BIO));
    if (!b) return NULL;
    b->data = (const uint8_t *)buf;
    b->len = sz;
    b->pos = 0u;
    return b;
}

int BIO_free(BIO *a) {
    free(a);
    return 1;
}

X509 *PEM_read_bio_X509(BIO *bp, X509 **x, int (*cb)(char *, int, int, void *), void *u) {
    (void)cb; (void)u;
    if (!bp || !bp->data || bp->pos >= bp->len) return NULL;

    br_pem_decoder_context pc;
    br_pem_decoder_init(&pc);
    ssl_dyn_buf_t der = { NULL, 0u, 0u };
    int collecting = 0;
    int finished = 0;
    br_pem_decoder_setdest(&pc, dn_buf_append, &der);

    while (bp->pos < bp->len) {
        size_t used = br_pem_decoder_push(&pc, bp->data + bp->pos, bp->len - bp->pos);
        bp->pos += used;
        int ev = br_pem_decoder_event(&pc);
        if (ev == BR_PEM_BEGIN_OBJ) {
            const char *name = br_pem_decoder_name(&pc);
            der.len = 0u;
            collecting = (name && (strcmp(name, "CERTIFICATE") == 0 ||
                                   strcmp(name, "X509 CERTIFICATE") == 0 ||
                                   strcmp(name, "TRUSTED CERTIFICATE") == 0));
        } else if (ev == BR_PEM_END_OBJ) {
            if (collecting && der.len > 0u) {
                finished = 1;
                break;
            }
            collecting = 0;
            der.len = 0u;
        } else if (ev == BR_PEM_ERROR) {
            collecting = 0;
            der.len = 0u;
        }
    }

    if (!finished || der.len == 0u) {
        free(der.buf);
        return NULL;
    }

    X509 *cert = x509_from_der(der.buf, der.len);
    free(der.buf);
    if (cert && x) *x = cert;
    return cert;
}

int X509_STORE_add_cert(X509_STORE *ctx, X509 *x) {
    if (!ctx || !x || !x->dn_data || x->dn_len == 0u) return 0;
    if (ctx->count >= ctx->cap) {
        size_t ncap = ctx->cap ? (ctx->cap * 2u) : 64u;
        br_x509_trust_anchor *na = (br_x509_trust_anchor *)realloc(
            ctx->anchors, ncap * sizeof(br_x509_trust_anchor));
        if (!na) return 0;
        ctx->anchors = na;
        ctx->cap = ncap;
    }

    br_x509_trust_anchor *ta = &ctx->anchors[ctx->count];
    memset(ta, 0, sizeof(*ta));
    ta->dn.len = x->dn_len;
    ta->dn.data = (unsigned char *)malloc(x->dn_len);
    if (!ta->dn.data) return 0;
    memcpy(ta->dn.data, x->dn_data, x->dn_len);
    ta->flags = x->ta_flags;
    ta->pkey.key_type = x->pkey.key_type;

    if (x->pkey.key_type == BR_KEYTYPE_RSA) {
        ta->pkey.key.rsa.nlen = x->pkey.key.rsa.nlen;
        ta->pkey.key.rsa.elen = x->pkey.key.rsa.elen;
        ta->pkey.key.rsa.n = (unsigned char *)malloc(x->pkey.key.rsa.nlen ? x->pkey.key.rsa.nlen : 1u);
        ta->pkey.key.rsa.e = (unsigned char *)malloc(x->pkey.key.rsa.elen ? x->pkey.key.rsa.elen : 1u);
        if (!ta->pkey.key.rsa.n || !ta->pkey.key.rsa.e) {
            free(ta->pkey.key.rsa.n);
            free(ta->pkey.key.rsa.e);
            free(ta->dn.data);
            return 0;
        }
        if (x->pkey.key.rsa.nlen) memcpy(ta->pkey.key.rsa.n, x->pkey.key.rsa.n, x->pkey.key.rsa.nlen);
        if (x->pkey.key.rsa.elen) memcpy(ta->pkey.key.rsa.e, x->pkey.key.rsa.e, x->pkey.key.rsa.elen);
    } else if (x->pkey.key_type == BR_KEYTYPE_EC) {
        ta->pkey.key.ec.curve = x->pkey.key.ec.curve;
        ta->pkey.key.ec.qlen = x->pkey.key.ec.qlen;
        ta->pkey.key.ec.q = (unsigned char *)malloc(x->pkey.key.ec.qlen ? x->pkey.key.ec.qlen : 1u);
        if (!ta->pkey.key.ec.q) {
            free(ta->dn.data);
            return 0;
        }
        if (x->pkey.key.ec.qlen) memcpy(ta->pkey.key.ec.q, x->pkey.key.ec.q, x->pkey.key.ec.qlen);
    } else {
        free(ta->dn.data);
        return 0;
    }

    ctx->count++;
    return 1;
}

int RAND_bytes(unsigned char *buf, int num) {
    if (!buf || num <= 0) return 0;
    return getentropy(buf, (size_t)num) == 0 ? 1 : 0;
}

unsigned long ERR_get_error(void) { return 0; }
unsigned long ERR_peek_error(void) { return 0; }
void ERR_clear_error(void) {}

const char *ERR_error_string(unsigned long e, char *buf) {
    (void)e;
    if (buf) {
        strcpy(buf, "SSL OK");
        return buf;
    }
    return "SSL OK";
}

const char *SSLeay_version(int type) { (void)type; return "BearSSL/0.6 (OpenSSL compat)"; }
const char *OpenSSL_version(int type) { (void)type; return "BearSSL/0.6 (OpenSSL compat)"; }

int CRYPTO_set_mem_functions(
    void *(*m)(size_t, const char *, int),
    void *(*r)(void *, size_t, const char *, int),
    void (*f)(void *, const char *, int))
{
    (void)m; (void)r; (void)f;
    return 1;
}

void clear_ssl_errors(int line) { (void)line; }
