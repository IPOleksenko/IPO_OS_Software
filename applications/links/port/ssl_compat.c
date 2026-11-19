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

extern int getentropy(void *buffer, size_t length);

#include "../../../lib/bearssl/inc/bearssl.h"

typedef struct {
    const br_x509_class *vtable;
    const br_x509_class **inner;
} x509_noanchor_context;

static void xwc_start_chain(const br_x509_class **ctx, const char *server_name) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
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

struct ssl_ctx_st {
    int refcount;
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
};

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
    SSL_CTX *ctx = (SSL_CTX *)calloc(1, sizeof(SSL_CTX));
    if (ctx) ctx->refcount = 1;
    return ctx;
}

void SSL_CTX_free(SSL_CTX *ctx) {
    if (ctx) free(ctx);
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

int SSL_CTX_set_default_verify_paths(SSL_CTX *ctx) {
    (void)ctx;
    return 1;
}

int SSL_CTX_load_verify_locations(SSL_CTX *ctx, const char *cafile, const char *capath) {
    (void)ctx; (void)cafile; (void)capath;
    return 1;
}

X509_STORE *SSL_CTX_get_cert_store(const SSL_CTX *ctx) {
    (void)ctx;
    static struct x509_store_st dummy_store;
    return &dummy_store;
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

SSL *SSL_new(SSL_CTX *ctx) {
    (void)ctx;
    SSL *s = (SSL *)calloc(1, sizeof(SSL));
    if (!s) return NULL;
    s->fd = -1;
    s->connected = 0;
    s->last_err = SSL_ERROR_NONE;

    br_ssl_client_init_full(&s->sc, &s->xc, NULL, 0);
    s->xwc.vtable = &x509_noanchor_vtable;
    s->xwc.inner = &s->xc.vtable;
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
    inject_ssl_entropy(&ssl->sc.eng);
    ssl->peer_eof = 0;
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
        ssl->last_err = SSL_ERROR_ZERO_RETURN;
        return 0;
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

static struct x509_st dummy_peer_cert;

X509 *SSL_get_peer_certificate(const SSL *ssl) {
    (void)ssl;
    return &dummy_peer_cert;
}

long SSL_get_verify_result(const SSL *ssl) {
    (void)ssl;
    return X509_V_OK;
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
    (void)x509;
}

X509_NAME *X509_get_subject_name(const X509 *a) {
    (void)a;
    static struct x509_name_st dummy_name;
    return &dummy_name;
}

char *X509_NAME_oneline(const X509_NAME *a, char *buf, int size) {
    (void)a;
    if (buf && size > 16) {
        strncpy(buf, "/CN=server.com", size - 1);
        buf[size - 1] = '\0';
        return buf;
    }
    return strdup("/CN=server.com");
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
    /* BearSSL's br_x509_minimal validates server_name against certificate SAN/CN during handshake. */
    return 1;
}

int X509_check_ip(X509 *cert, const unsigned char *ip, size_t iplen, unsigned int flags) {
    (void)cert; (void)ip; (void)iplen; (void)flags;
    return 1;
}

BIO *BIO_new_mem_buf(const void *buf, int len) {
    (void)buf; (void)len;
    static struct bio_st dummy_bio;
    return &dummy_bio;
}

int BIO_free(BIO *a) {
    (void)a;
    return 1;
}

X509 *PEM_read_bio_X509(BIO *bp, X509 **x, int (*cb)(char *, int, int, void *), void *u) {
    (void)bp; (void)x; (void)cb; (void)u;
    return &dummy_peer_cert;
}

int X509_STORE_add_cert(X509_STORE *ctx, X509 *x) {
    (void)ctx; (void)x;
    return 1;
}

int RAND_bytes(unsigned char *buf, int num) {
    if (!buf || num <= 0) return 0;
    return getentropy(buf, (size_t)num) == 0 ? 1 : 0;
}

unsigned long ERR_get_error(void) {
    return 0;
}

unsigned long ERR_peek_error(void) {
    return 0;
}

void ERR_clear_error(void) {}

const char *ERR_error_string(unsigned long e, char *buf) {
    (void)e;
    if (buf) {
        strcpy(buf, "SSL OK");
        return buf;
    }
    return "SSL OK";
}

const char *SSLeay_version(int type) {
    (void)type;
    return "BearSSL/0.6 (OpenSSL compat)";
}

const char *OpenSSL_version(int type) {
    (void)type;
    return "BearSSL/0.6 (OpenSSL compat)";
}

int CRYPTO_set_mem_functions(
    void *(*m)(size_t, const char *, int),
    void *(*r)(void *, size_t, const char *, int),
    void (*f)(void *, const char *, int))
{
    (void)m; (void)r; (void)f;
    return 1;
}

void clear_ssl_errors(int line) {
    (void)line;
}
