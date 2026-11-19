#ifndef OPENSSL_CRYPTO_H
#define OPENSSL_CRYPTO_H
#include <stddef.h>

#define SSLEAY_VERSION 0
#define OPENSSL_VERSION 0

const char *SSLeay_version(int type);
const char *OpenSSL_version(int type);

int CRYPTO_set_mem_functions(
    void *(*m)(size_t, const char *, int),
    void *(*r)(void *, size_t, const char *, int),
    void (*f)(void *, const char *, int));
#endif
