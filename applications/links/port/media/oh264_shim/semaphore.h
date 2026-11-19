#ifndef IPO_OH264_SEMAPHORE_SHIM_H
#define IPO_OH264_SEMAPHORE_SHIM_H

#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int sem_t;

static inline int sem_init(sem_t *s, int pshared, unsigned val) { (void)pshared; if (s) *s = (int)val; return 0; }
static inline int sem_destroy(sem_t *s) { (void)s; return 0; }
static inline int sem_wait(sem_t *s) { (void)s; return 0; }
static inline int sem_timedwait(sem_t *s, const struct timespec *ts) { (void)s; (void)ts; return 0; }
static inline int sem_post(sem_t *s) { (void)s; return 0; }

#ifdef __cplusplus
}
#endif

#endif
