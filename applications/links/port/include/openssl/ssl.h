#ifndef OPENSSL_SSL_H
#define OPENSSL_SSL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SSL_ERROR_NONE             0
#define SSL_ERROR_SSL              1
#define SSL_ERROR_WANT_READ        2
#define SSL_ERROR_WANT_WRITE       3
#define SSL_ERROR_SYSCALL          5
#define SSL_ERROR_ZERO_RETURN      6

#define SSL_OP_ALL                 0x000FFFFFu
#define SSL_OP_NO_COMPRESSION      0x00020000u
#define SSL_OP_NO_SSLv2            0x01000000u
#define SSL_OP_NO_SSLv3            0x02000000u
#define SSL_OP_NO_TLSv1            0x04000000u
#define SSL_OP_NO_TLSv1_1          0x10000000u
#define SSL_OP_NO_TLSv1_2          0x08000000u
#define SSL_OP_NO_TLSv1_3          0x20000000u

#define SSL_MODE_ENABLE_PARTIAL_WRITE 0x01u
#define SSL_MODE_SEND_FALLBACK_SCSV   0x02u

#define SSL_FILETYPE_PEM           1

#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55

#define SSL3_VERSION               0x0300
#define TLS1_VERSION               0x0301
#define TLS1_1_VERSION             0x0302
#define TLS1_2_VERSION             0x0303
#define TLS1_3_VERSION             0x0304

#define X509_V_OK                  0

struct ssl_ctx_st;
struct ssl_st;
struct ssl_method_st { int dummy; };
struct ssl_session_st { int dummy; };
struct x509_st { int dummy; };
struct x509_store_st { int dummy; };
struct x509_name_st { int dummy; };
struct x509_name_entry_st { int dummy; };
struct bio_st { int dummy; };
struct asn1_string_st { int dummy; };

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct ssl_method_st SSL_METHOD;
typedef struct ssl_session_st SSL_SESSION;
typedef struct x509_st X509;
typedef struct x509_store_st X509_STORE;
typedef struct x509_name_st X509_NAME;
typedef struct x509_name_entry_st X509_NAME_ENTRY;
typedef struct bio_st BIO;
typedef struct asn1_string_st ASN1_STRING;

int OPENSSL_init_ssl(uint64_t opts, const void *settings);
int OpenSSL_add_ssl_algorithms(void);
int SSL_library_init(void);
void SSL_load_error_strings(void);
void OPENSSL_cleanup(void);

const SSL_METHOD *TLS_client_method(void);
const SSL_METHOD *SSLv23_client_method(void);

SSL_CTX *SSL_CTX_new(const SSL_METHOD *meth);
void SSL_CTX_free(SSL_CTX *ctx);
long SSL_CTX_set_options(SSL_CTX *ctx, long options);
long SSL_CTX_set_mode(SSL_CTX *ctx, long mode);
int SSL_CTX_set_min_proto_version(SSL_CTX *ctx, int version);
int SSL_CTX_set_default_verify_paths(SSL_CTX *ctx);
int SSL_CTX_load_verify_locations(SSL_CTX *ctx, const char *cafile, const char *capath);
X509_STORE *SSL_CTX_get_cert_store(const SSL_CTX *ctx);
void SSL_CTX_set_default_passwd_cb(SSL_CTX *ctx, int (*cb)(char *, int, int, void *));

SSL *SSL_new(SSL_CTX *ctx);
void SSL_free(SSL *ssl);
int SSL_set_fd(SSL *ssl, int fd);
long SSL_set_tlsext_host_name(SSL *ssl, const char *name);
long SSL_set_options(SSL *ssl, long options);
long SSL_set_mode(SSL *ssl, long mode);
void SSL_set_security_level(SSL *ssl, int level);
void SSL_set_quiet_shutdown(SSL *ssl, int mode);
int SSL_shutdown(SSL *ssl);
int SSL_connect(SSL *ssl);
int SSL_get_error(const SSL *ssl, int ret);
int SSL_write(SSL *ssl, const void *buf, int num);
int SSL_read(SSL *ssl, void *buf, int num);
const char *SSL_get_version(const SSL *ssl);
const char *SSL_get_cipher_name(const SSL *ssl);
int SSL_get_cipher_bits(const SSL *ssl, int *alg_bits);
X509 *SSL_get_peer_certificate(const SSL *ssl);
long SSL_get_verify_result(const SSL *ssl);
int SSL_session_reused(const SSL *ssl);
int SSL_set_session(SSL *ssl, SSL_SESSION *session);
void SSL_SESSION_free(SSL_SESSION *ses);

void X509_free(X509 *x509);
X509_NAME *X509_get_subject_name(const X509 *a);
char *X509_NAME_oneline(const X509_NAME *a, char *buf, int size);
int X509_NAME_get_index_by_NID(const X509_NAME *name, int nid, int lastpos);
X509_NAME_ENTRY *X509_NAME_get_entry(const X509_NAME *name, int loc);
ASN1_STRING *X509_NAME_ENTRY_get_data(const X509_NAME_ENTRY *ne);
const unsigned char *ASN1_STRING_get0_data(const ASN1_STRING *x);
unsigned char *ASN1_STRING_data(ASN1_STRING *x);
int ASN1_STRING_length(const ASN1_STRING *x);
int X509_check_host(X509 *cert, const char *name, size_t namelen, unsigned int flags, char **peername);
int X509_check_ip(X509 *cert, const unsigned char *ip, size_t iplen, unsigned int flags);

BIO *BIO_new_mem_buf(const void *buf, int len);
int BIO_free(BIO *a);
X509 *PEM_read_bio_X509(BIO *bp, X509 **x, int (*cb)(char *, int, int, void *), void *u);
int X509_STORE_add_cert(X509_STORE *ctx, X509 *x);

#endif
