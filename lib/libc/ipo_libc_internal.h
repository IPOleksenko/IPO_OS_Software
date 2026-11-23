#ifndef IPO_LIBC_INTERNAL_H
#define IPO_LIBC_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <setjmp.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ipo_libc_tcb {
    int errno_val;
    char *strtok_last;
    struct tm tm_buf;
    char *asctime_buf;
    size_t asctime_cap;
    char *dlerror_buf;
    int has_dlerror;
    void **tls_vals;
    size_t tls_cap;
    int has_exit_jmp;
    jmp_buf exit_jmp;
    void *exit_val;
    uint32_t tid;
} ipo_libc_tcb_t;

ipo_libc_tcb_t *ipo_libc_get_tcb(void);
void ipo_libc_raw_futex_lock(volatile uint32_t *state);
void ipo_libc_raw_futex_unlock(volatile uint32_t *state);
void ipo_libc_run_atexit_and_fini(void);

#ifdef __cplusplus
}
#endif

#endif /* IPO_LIBC_INTERNAL_H */
