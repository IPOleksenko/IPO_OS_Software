#include <pthread.h>
#include <threads.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <syscall.h>
#include "ipo_libc_internal.h"

#undef errno
int errno = 0;

static ipo_libc_tcb_t s_main_tcb;
static volatile uint32_t s_main_tid = 0u;

typedef void (*tls_dtor_fn_t)(void *);

typedef struct {
    int in_use;
    tls_dtor_fn_t dtor;
} tls_key_entry_t;

static tls_key_entry_t *s_tls_keys = NULL;
static size_t s_tls_keys_cap = 0u;
static pthread_mutex_t s_tls_keys_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint32_t current_kernel_tid(void) {
    uint32_t tid = (uint32_t)ipo_syscall(IPO_SYSCALL_THREAD_SELF, 0u, NULL);
    return (tid != 0u) ? tid : 1u;
}

static void free_tcb_resources(ipo_libc_tcb_t *tcb) {
    if (!tcb) return;
    for (int iter = 0; iter < PTHREAD_DESTRUCTOR_ITERATIONS; iter++) {
        int called = 0;
        for (size_t i = 0u; i < tcb->tls_cap; i++) {
            void *val = tcb->tls_vals ? tcb->tls_vals[i] : NULL;
            if (!val) continue;
            tls_dtor_fn_t dtor = NULL;
            pthread_mutex_lock(&s_tls_keys_lock);
            if (i < s_tls_keys_cap && s_tls_keys[i].in_use) {
                dtor = s_tls_keys[i].dtor;
            }
            pthread_mutex_unlock(&s_tls_keys_lock);
            if (dtor) {
                tcb->tls_vals[i] = NULL;
                dtor(val);
                called = 1;
            }
        }
        if (!called) break;
    }
    if (tcb->tls_vals) {
        free(tcb->tls_vals);
        tcb->tls_vals = NULL;
        tcb->tls_cap = 0u;
    }
    if (tcb->asctime_buf) {
        free(tcb->asctime_buf);
        tcb->asctime_buf = NULL;
        tcb->asctime_cap = 0u;
    }
    if (tcb->dlerror_buf) {
        free(tcb->dlerror_buf);
        tcb->dlerror_buf = NULL;
    }
}

ipo_libc_tcb_t *ipo_libc_get_tcb(void) {
    uintptr_t tls_raw = ipo_thread_get_tls();
    if (tls_raw != 0u) {
        return (ipo_libc_tcb_t *)tls_raw;
    }

    uint32_t tid = current_kernel_tid();
    uint32_t expected = 0u;
    if (__atomic_load_n(&s_main_tid, __ATOMIC_ACQUIRE) == 0u) {
        __atomic_compare_exchange_n(&s_main_tid, &expected, tid, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }

    if (tid == __atomic_load_n(&s_main_tid, __ATOMIC_ACQUIRE)) {
        s_main_tcb.tid = tid;
        ipo_thread_set_tls((uintptr_t)&s_main_tcb);
        return &s_main_tcb;
    }

    ipo_libc_tcb_t *worker = (ipo_libc_tcb_t *)calloc(1, sizeof(ipo_libc_tcb_t));
    if (!worker) {
        return &s_main_tcb;
    }
    worker->tid = tid;
    ipo_thread_set_tls((uintptr_t)worker);
    return worker;
}

int *__errno_location(void) {
    return &ipo_libc_get_tcb()->errno_val;
}

void ipo_libc_raw_futex_lock(volatile uint32_t *state) {
    uint32_t c = 0u;
    if (__atomic_compare_exchange_n(state, &c, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return;
    }
    if (c != 2u) {
        c = __atomic_exchange_n(state, 2u, __ATOMIC_ACQUIRE);
    }
    while (c != 0u) {
        ipo_futex(state, IPO_FUTEX_WAIT, 2u, 0u);
        c = __atomic_exchange_n(state, 2u, __ATOMIC_ACQUIRE);
    }
}

void ipo_libc_raw_futex_unlock(volatile uint32_t *state) {
    if (__atomic_fetch_sub(state, 1u, __ATOMIC_RELEASE) != 1u) {
        __atomic_store_n(state, 0u, __ATOMIC_RELEASE);
        ipo_futex(state, IPO_FUTEX_WAKE, 1u, 0u);
    }
}

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
    uint32_t detached;
    volatile uint32_t ready;
    pthread_t tid;
} pthread_launch_ctx_t;

static void *pthread_entry_trampoline(void *raw) {
    pthread_launch_ctx_t *ctx = (pthread_launch_ctx_t *)raw;
    void *(*fn)(void *) = ctx->start_routine;
    void *arg = ctx->arg;
    free(ctx);

    ipo_libc_tcb_t *tcb = (ipo_libc_tcb_t *)calloc(1, sizeof(ipo_libc_tcb_t));
    if (tcb) {
        tcb->tid = current_kernel_tid();
        ipo_thread_set_tls((uintptr_t)tcb);
        tcb->has_exit_jmp = 1;
        if (setjmp(tcb->exit_jmp) == 0) {
            tcb->exit_val = fn(arg);
        }
        void *ret = tcb->exit_val;
        free_tcb_resources(tcb);
        ipo_thread_set_tls(0u);
        free(tcb);
        return ret;
    }
    return fn(arg);
}

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    attr->stack_size = 0u;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    return attr ? 0 : EINVAL;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
    if (!attr || (detachstate != (int)PTHREAD_CREATE_JOINABLE &&
                  detachstate != (int)PTHREAD_CREATE_DETACHED)) {
        return EINVAL;
    }
    attr->detachstate = (uint32_t)detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate) {
    if (!attr || !detachstate) return EINVAL;
    *detachstate = (int)attr->detachstate;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
    if (!attr) return EINVAL;
    attr->stack_size = stacksize;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize) {
    if (!attr || !stacksize) return EINVAL;
    *stacksize = attr->stack_size;
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
    if (!thread || !start_routine) return EINVAL;
    /* Anchor main_tid before spawning first worker */
    (void)ipo_libc_get_tcb();

    pthread_launch_ctx_t *ctx = (pthread_launch_ctx_t *)calloc(1, sizeof(pthread_launch_ctx_t));
    if (!ctx) return EAGAIN;
    ctx->start_routine = start_routine;
    ctx->arg = arg;
    ctx->detached = (attr && attr->detachstate == PTHREAD_CREATE_DETACHED) ? 1u : 0u;

    uint32_t tid = 0u;
    uint32_t args[3] = {
        (uint32_t)(uintptr_t)pthread_entry_trampoline,
        (uint32_t)(uintptr_t)ctx,
        (uint32_t)(uintptr_t)&tid
    };
    int rc = ipo_syscall(IPO_SYSCALL_THREAD_CREATE, 3u, args);
    if (rc != 0 || tid == 0u) {
        free(ctx);
        return EAGAIN;
    }
    if (ctx->detached) {
        ipo_thread_detach(tid);
    }
    *thread = tid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    if (thread == 0u) return ESRCH;
    if (thread == pthread_self()) return EDEADLK;
    uintptr_t res_word = 0u;
    uint32_t args[2] = { thread, (uint32_t)(uintptr_t)&res_word };
    int rc = ipo_syscall(IPO_SYSCALL_THREAD_JOIN, 2u, args);
    if (rc != 0) return EINVAL;
    if (retval) *retval = (void *)res_word;
    return 0;
}

int pthread_detach(pthread_t thread) {
    if (thread == 0u) return ESRCH;
    return (ipo_thread_detach(thread) == 0) ? 0 : EINVAL;
}

void pthread_exit(void *retval) {
    ipo_libc_tcb_t *tcb = ipo_libc_get_tcb();
    if (tcb && tcb != &s_main_tcb && tcb->has_exit_jmp) {
        tcb->exit_val = retval;
        longjmp(tcb->exit_jmp, 1);
    }
    free_tcb_resources(&s_main_tcb);
    uint32_t args[1] = { (uint32_t)(uintptr_t)retval };
    ipo_syscall(IPO_SYSCALL_THREAD_EXIT, 1u, args);
    for (;;) {}
}

pthread_t pthread_self(void) {
    return current_kernel_tid();
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return t1 == t2;
}

int pthread_setname_np(pthread_t thread, const char *name) {
    (void)thread;
    (void)name;
    return 0;
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    attr->type = PTHREAD_MUTEX_NORMAL;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    return attr ? 0 : EINVAL;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr || type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_ERRORCHECK) {
        return EINVAL;
    }
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
    if (!attr || !type) return EINVAL;
    *type = attr->type;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr) {
    if (!mutex) return EINVAL;
    mutex->state = 0u;
    mutex->owner = 0u;
    mutex->type = attr ? attr->type : PTHREAD_MUTEX_NORMAL;
    mutex->rec_count = 0u;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    if (__atomic_load_n(&mutex->state, __ATOMIC_ACQUIRE) != 0u) return EBUSY;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    uint32_t self = current_kernel_tid();
    if ((mutex->type == PTHREAD_MUTEX_RECURSIVE || mutex->type == PTHREAD_MUTEX_ERRORCHECK) &&
        __atomic_load_n(&mutex->state, __ATOMIC_RELAXED) != 0u &&
        __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == self) {
        if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
            mutex->rec_count++;
            return 0;
        }
        return EDEADLK;
    }
    ipo_libc_raw_futex_lock(&mutex->state);
    __atomic_store_n(&mutex->owner, self, __ATOMIC_RELAXED);
    mutex->rec_count = 1u;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    uint32_t self = current_kernel_tid();
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE &&
        __atomic_load_n(&mutex->state, __ATOMIC_RELAXED) != 0u &&
        __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == self) {
        mutex->rec_count++;
        return 0;
    }
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(&mutex->state, &expected, 1u, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_store_n(&mutex->owner, self, __ATOMIC_RELAXED);
        mutex->rec_count = 1u;
        return 0;
    }
    return EBUSY;
}

static int abs_to_rel_timespec(int clock_id, const struct timespec *abstime, ipo_timespec_t *out_rel) {
    if (!abstime || abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000L) {
        return EINVAL;
    }
    struct timespec now;
    clock_gettime(clock_id, &now);
    int64_t ds = (int64_t)abstime->tv_sec - (int64_t)now.tv_sec;
    int64_t dns = (int64_t)abstime->tv_nsec - (int64_t)now.tv_nsec;
    if (dns < 0) {
        ds -= 1;
        dns += 1000000000LL;
    }
    if (ds < 0 || (ds == 0 && dns <= 0)) {
        return ETIMEDOUT;
    }
    out_rel->tv_sec = (uint64_t)ds;
    out_rel->tv_nsec = (uint64_t)dns;
    return 0;
}

int pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *abstime) {
    if (!mutex || !abstime) return EINVAL;
    int rc = pthread_mutex_trylock(mutex);
    if (rc == 0) return 0;

    uint32_t self = current_kernel_tid();
    if (mutex->type == PTHREAD_MUTEX_ERRORCHECK &&
        __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) == self) {
        return EDEADLK;
    }

    uint32_t c = __atomic_exchange_n(&mutex->state, 2u, __ATOMIC_ACQUIRE);
    while (c != 0u) {
        ipo_timespec_t rel;
        int tr = abs_to_rel_timespec(CLOCK_REALTIME, abstime, &rel);
        if (tr != 0) return tr;
        int f = ipo_futex(&mutex->state, IPO_FUTEX_WAIT_TS, 2u, (uint32_t)(uintptr_t)&rel);
        c = __atomic_exchange_n(&mutex->state, 2u, __ATOMIC_ACQUIRE);
        if (c == 0u) break;
        if (f == -2) return ETIMEDOUT;
    }
    __atomic_store_n(&mutex->owner, self, __ATOMIC_RELAXED);
    mutex->rec_count = 1u;
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    uint32_t self = current_kernel_tid();
    if (mutex->type == PTHREAD_MUTEX_RECURSIVE) {
        if (__atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != self) return EPERM;
        if (mutex->rec_count > 1u) {
            mutex->rec_count--;
            return 0;
        }
    } else if (mutex->type == PTHREAD_MUTEX_ERRORCHECK) {
        if (__atomic_load_n(&mutex->state, __ATOMIC_RELAXED) == 0u ||
            __atomic_load_n(&mutex->owner, __ATOMIC_RELAXED) != self) {
            return EPERM;
        }
    }
    __atomic_store_n(&mutex->owner, 0u, __ATOMIC_RELAXED);
    mutex->rec_count = 0u;
    ipo_libc_raw_futex_unlock(&mutex->state);
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr) {
    if (!attr) return EINVAL;
    attr->clock_id = CLOCK_REALTIME;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
    return attr ? 0 : EINVAL;
}

int pthread_condattr_setclock(pthread_condattr_t *attr, int clock_id) {
    if (!attr || (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)) return EINVAL;
    attr->clock_id = clock_id;
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    (void)attr;
    if (!cond) return EINVAL;
    cond->seq = 0u;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    return cond ? 0 : EINVAL;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (!cond || !mutex) return EINVAL;
    uint32_t seq = __atomic_load_n(&cond->seq, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(mutex);
    ipo_futex(&cond->seq, IPO_FUTEX_WAIT, seq, 0u);
    pthread_mutex_lock(mutex);
    return 0;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    if (!cond || !mutex || !abstime) return EINVAL;
    ipo_timespec_t rel;
    int tr = abs_to_rel_timespec(CLOCK_REALTIME, abstime, &rel);
    if (tr != 0) return tr;
    uint32_t seq = __atomic_load_n(&cond->seq, __ATOMIC_ACQUIRE);
    pthread_mutex_unlock(mutex);
    int rc = ipo_futex(&cond->seq, IPO_FUTEX_WAIT_TS, seq, (uint32_t)(uintptr_t)&rel);
    pthread_mutex_lock(mutex);
    if (rc == -2 && __atomic_load_n(&cond->seq, __ATOMIC_ACQUIRE) == seq) {
        return ETIMEDOUT;
    }
    return 0;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    __atomic_add_fetch(&cond->seq, 1u, __ATOMIC_RELEASE);
    ipo_futex(&cond->seq, IPO_FUTEX_WAKE, 1u, 0u);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    __atomic_add_fetch(&cond->seq, 1u, __ATOMIC_RELEASE);
    ipo_futex(&cond->seq, IPO_FUTEX_WAKE, 0x7FFFFFFFu, 0u);
    return 0;
}

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void)) {
    if (!once_control || !init_routine) return EINVAL;
    if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 2u) return 0;

    for (;;) {
        uint32_t state = 0u;
        if (__atomic_compare_exchange_n(once_control, &state, 1u, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            init_routine();
            __atomic_store_n(once_control, 2u, __ATOMIC_RELEASE);
            ipo_futex(once_control, IPO_FUTEX_WAKE, 0x7FFFFFFFu, 0u);
            return 0;
        }
        if (state == 2u) {
            return 0;
        }
        ipo_futex(once_control, IPO_FUTEX_WAIT, 1u, 0u);
    }
}

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *)) {
    if (!key) return EINVAL;
    pthread_mutex_lock(&s_tls_keys_lock);
    for (size_t i = 0u; i < s_tls_keys_cap; i++) {
        if (!s_tls_keys[i].in_use) {
            s_tls_keys[i].in_use = 1;
            s_tls_keys[i].dtor = destructor;
            *key = (pthread_key_t)i;
            pthread_mutex_unlock(&s_tls_keys_lock);
            return 0;
        }
    }
    size_t ncap = s_tls_keys_cap ? (s_tls_keys_cap * 2u) : 32u;
    if (ncap < s_tls_keys_cap || ncap > SIZE_MAX / sizeof(tls_key_entry_t)) {
        pthread_mutex_unlock(&s_tls_keys_lock);
        return EAGAIN;
    }
    tls_key_entry_t *narr = (tls_key_entry_t *)realloc(s_tls_keys, ncap * sizeof(tls_key_entry_t));
    if (!narr) {
        pthread_mutex_unlock(&s_tls_keys_lock);
        return ENOMEM;
    }
    memset(narr + s_tls_keys_cap, 0, (ncap - s_tls_keys_cap) * sizeof(tls_key_entry_t));
    s_tls_keys = narr;
    size_t idx = s_tls_keys_cap;
    s_tls_keys_cap = ncap;
    s_tls_keys[idx].in_use = 1;
    s_tls_keys[idx].dtor = destructor;
    *key = (pthread_key_t)idx;
    pthread_mutex_unlock(&s_tls_keys_lock);
    return 0;
}

int pthread_key_delete(pthread_key_t key) {
    pthread_mutex_lock(&s_tls_keys_lock);
    if ((size_t)key >= s_tls_keys_cap || !s_tls_keys[key].in_use) {
        pthread_mutex_unlock(&s_tls_keys_lock);
        return EINVAL;
    }
    s_tls_keys[key].in_use = 0;
    s_tls_keys[key].dtor = NULL;
    pthread_mutex_unlock(&s_tls_keys_lock);
    return 0;
}

void *pthread_getspecific(pthread_key_t key) {
    ipo_libc_tcb_t *tcb = ipo_libc_get_tcb();
    if (!tcb || (size_t)key >= tcb->tls_cap || !tcb->tls_vals) return NULL;
    return tcb->tls_vals[key];
}

int pthread_setspecific(pthread_key_t key, const void *value) {
    pthread_mutex_lock(&s_tls_keys_lock);
    int valid = ((size_t)key < s_tls_keys_cap && s_tls_keys[key].in_use);
    pthread_mutex_unlock(&s_tls_keys_lock);
    if (!valid) return EINVAL;

    ipo_libc_tcb_t *tcb = ipo_libc_get_tcb();
    if (!tcb) return ENOMEM;
    if ((size_t)key >= tcb->tls_cap) {
        size_t ncap = tcb->tls_cap ? tcb->tls_cap : 32u;
        while (ncap <= (size_t)key) ncap *= 2u;
        void **narr = (void **)realloc(tcb->tls_vals, ncap * sizeof(void *));
        if (!narr) return ENOMEM;
        memset(narr + tcb->tls_cap, 0, (ncap - tcb->tls_cap) * sizeof(void *));
        tcb->tls_vals = narr;
        tcb->tls_cap = ncap;
    }
    tcb->tls_vals[key] = (void *)value;
    return 0;
}

int pthread_rwlock_init(pthread_rwlock_t *rwlock, const pthread_rwlockattr_t *attr) {
    (void)attr;
    if (!rwlock) return EINVAL;
    rwlock->state = 0;
    rwlock->writer_waiters = 0u;
    return 0;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;
    return (__atomic_load_n(&rwlock->state, __ATOMIC_ACQUIRE) == 0) ? 0 : EBUSY;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;
    for (;;) {
        int32_t st = __atomic_load_n(&rwlock->state, __ATOMIC_ACQUIRE);
        if (st >= 0 && __atomic_load_n(&rwlock->writer_waiters, __ATOMIC_ACQUIRE) == 0u) {
            if (__atomic_compare_exchange_n(&rwlock->state, &st, st + 1, false,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                return 0;
            }
            continue;
        }
        ipo_futex((volatile uint32_t *)&rwlock->state, IPO_FUTEX_WAIT, (uint32_t)st, 0u);
    }
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;
    int32_t st = __atomic_load_n(&rwlock->state, __ATOMIC_ACQUIRE);
    while (st >= 0 && __atomic_load_n(&rwlock->writer_waiters, __ATOMIC_ACQUIRE) == 0u) {
        if (__atomic_compare_exchange_n(&rwlock->state, &st, st + 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }
    }
    return EBUSY;
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;
    __atomic_add_fetch(&rwlock->writer_waiters, 1u, __ATOMIC_ACQUIRE);
    for (;;) {
        int32_t expected = 0;
        if (__atomic_compare_exchange_n(&rwlock->state, &expected, -1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            __atomic_sub_fetch(&rwlock->writer_waiters, 1u, __ATOMIC_RELEASE);
            return 0;
        }
        ipo_futex((volatile uint32_t *)&rwlock->state, IPO_FUTEX_WAIT, (uint32_t)expected, 0u);
    }
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;
    int32_t expected = 0;
    return __atomic_compare_exchange_n(&rwlock->state, &expected, -1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : EBUSY;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock) {
    if (!rwlock) return EINVAL;
    int32_t st = __atomic_load_n(&rwlock->state, __ATOMIC_RELAXED);
    if (st == -1) {
        __atomic_store_n(&rwlock->state, 0, __ATOMIC_RELEASE);
        ipo_futex((volatile uint32_t *)&rwlock->state, IPO_FUTEX_WAKE, 0x7FFFFFFFu, 0u);
    } else if (st > 0) {
        if (__atomic_sub_fetch(&rwlock->state, 1, __ATOMIC_RELEASE) == 0) {
            ipo_futex((volatile uint32_t *)&rwlock->state, IPO_FUTEX_WAKE, 0x7FFFFFFFu, 0u);
        }
    } else {
        return EPERM;
    }
    return 0;
}

void (atomic_thread_fence)(memory_order order) {
    __atomic_thread_fence(order);
}

void (atomic_signal_fence)(memory_order order) {
    __atomic_signal_fence(order);
}

bool (__atomic_is_lock_free)(size_t size, void *ptr) {
    (void)ptr;
    return size == 1u || size == 2u || size == 4u;
}

bool atomic_flag_test_and_set_explicit(void *object, memory_order order) {
    return __atomic_test_and_set(object, (int)order);
}

bool atomic_flag_test_and_set(void *object) {
    return __atomic_test_and_set(object, __ATOMIC_SEQ_CST);
}

void atomic_flag_clear_explicit(void *object, memory_order order) {
    __atomic_clear(object, (int)order);
}

void atomic_flag_clear(void *object) {
    __atomic_clear(object, __ATOMIC_SEQ_CST);
}
