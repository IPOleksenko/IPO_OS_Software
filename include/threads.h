/*
 * Userspace kernel-backed thread, mutex, semaphore, and condition variable primitives.
 */

#ifndef IPO_THREADS_H
#define IPO_THREADS_H

#include <stdint.h>
#include <syscall.h>

typedef uint32_t ipo_thread_t;
typedef void *(*ipo_thread_start_t)(void *);

typedef struct { uint32_t opaque; } ipo_mutex_t;
typedef struct { uint32_t opaque; } ipo_sem_t;
typedef struct { uint32_t opaque; } ipo_cond_t;

#define IPO_MUTEX_INITIALIZER {0u}
#define IPO_SEM_INITIALIZER   {0u}
#define IPO_COND_INITIALIZER  {0u}

#define IPO_SYNC_MUTEX     1u
#define IPO_SYNC_SEMAPHORE 2u
#define IPO_SYNC_CONDITION 3u
#define IPO_SYNC_INIT      1u
#define IPO_SYNC_DESTROY   2u
#define IPO_SYNC_LOCK      3u
#define IPO_SYNC_UNLOCK    4u
#define IPO_SYNC_WAIT      5u
#define IPO_SYNC_POST      6u
#define IPO_SYNC_COND_WAIT 7u
#define IPO_SYNC_SIGNAL         8u
#define IPO_SYNC_BROADCAST      9u
#define IPO_SYNC_TRYLOCK        10u
#define IPO_SYNC_TIMED_LOCK     11u
#define IPO_SYNC_COND_TIMEDWAIT 12u
#define IPO_SYNC_TIMED_WAIT     13u

#define IPO_MUTEX_RECURSIVE     1u

static inline int ipo_thread_create(ipo_thread_t *thread,
                                   ipo_thread_start_t start,
                                   void *argument) {
    if (!thread || !start) return -1;
    uint32_t args[3] = {
        (uint32_t)(uintptr_t)start,
        (uint32_t)(uintptr_t)argument,
        (uint32_t)(uintptr_t)thread
    };
    return ipo_syscall(IPO_SYSCALL_THREAD_CREATE, 3u, args);
}

static inline int ipo_thread_join(ipo_thread_t thread, void **result) {
    if (!result) return -1;
    uint32_t args[2] = {thread, (uint32_t)(uintptr_t)result};
    return ipo_syscall(IPO_SYSCALL_THREAD_JOIN, 2u, args);
}

static inline int ipo_thread_detach(ipo_thread_t thread) {
    uint32_t args[1] = {thread};
    return ipo_syscall(IPO_SYSCALL_THREAD_DETACH, 1u, args);
}

static inline int ipo_thread_set_tls(uintptr_t tls_base) {
    uint32_t args[1] = {(uint32_t)tls_base};
    return ipo_syscall(IPO_SYSCALL_THREAD_SET_TLS, 1u, args);
}

static inline uintptr_t ipo_thread_get_tls(void) {
    return (uintptr_t)(uint32_t)ipo_syscall(IPO_SYSCALL_THREAD_GET_TLS, 0u, (uint32_t *)0);
}

static inline uint32_t ipo_thread_self(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_THREAD_SELF, 0u, (uint32_t *)0);
}

static inline void ipo_thread_yield(void) {
    (void)ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, (uint32_t *)0);
}

static inline void __attribute__((noreturn)) ipo_thread_exit(void *result) {
    uint32_t args[1] = {(uint32_t)(uintptr_t)result};
    (void)ipo_syscall(IPO_SYSCALL_THREAD_EXIT, 1u, args);
    for (;;) ipo_thread_yield();
}

static inline int ipo_sync_call(const void *object, uint32_t type,
                                uint32_t operation, uint32_t value) {
    uint32_t args[4] = {
        (uint32_t)(uintptr_t)object, type, operation, value
    };
    return ipo_syscall(IPO_SYSCALL_THREAD_SYNC, 4u, args);
}

static inline int ipo_mutex_init(ipo_mutex_t *mutex) {
    return mutex ? ipo_sync_call(mutex, IPO_SYNC_MUTEX, IPO_SYNC_INIT, 0u) : -1;
}
static inline int ipo_mutex_init_recursive(ipo_mutex_t *mutex) {
    return mutex ? ipo_sync_call(mutex, IPO_SYNC_MUTEX, IPO_SYNC_INIT, IPO_MUTEX_RECURSIVE) : -1;
}
static inline int ipo_mutex_destroy(ipo_mutex_t *mutex) {
    return mutex ? ipo_sync_call(mutex, IPO_SYNC_MUTEX, IPO_SYNC_DESTROY, 0u) : -1;
}
static inline int ipo_mutex_lock(ipo_mutex_t *mutex) {
    return mutex ? ipo_sync_call(mutex, IPO_SYNC_MUTEX, IPO_SYNC_LOCK, 0u) : -1;
}
static inline int ipo_mutex_trylock(ipo_mutex_t *mutex) {
    return mutex ? ipo_sync_call(mutex, IPO_SYNC_MUTEX, IPO_SYNC_TRYLOCK, 0u) : -1;
}
static inline int ipo_mutex_timedlock(ipo_mutex_t *mutex, uint32_t timeout_ms) {
    return mutex ? ipo_sync_call(mutex, IPO_SYNC_MUTEX, IPO_SYNC_TIMED_LOCK, timeout_ms) : -1;
}
static inline int ipo_mutex_unlock(ipo_mutex_t *mutex) {
    return mutex ? ipo_sync_call(mutex, IPO_SYNC_MUTEX, IPO_SYNC_UNLOCK, 0u) : -1;
}

static inline int ipo_sem_init(ipo_sem_t *semaphore, uint32_t value) {
    return semaphore ? ipo_sync_call(semaphore, IPO_SYNC_SEMAPHORE,
                                     IPO_SYNC_INIT, value) : -1;
}
static inline int ipo_sem_destroy(ipo_sem_t *semaphore) {
    return semaphore ? ipo_sync_call(semaphore, IPO_SYNC_SEMAPHORE,
                                     IPO_SYNC_DESTROY, 0u) : -1;
}
static inline int ipo_sem_wait(ipo_sem_t *semaphore) {
    return semaphore ? ipo_sync_call(semaphore, IPO_SYNC_SEMAPHORE,
                                     IPO_SYNC_WAIT, 0u) : -1;
}
static inline int ipo_sem_trywait(ipo_sem_t *semaphore) {
    return semaphore ? ipo_sync_call(semaphore, IPO_SYNC_SEMAPHORE,
                                     IPO_SYNC_TRYLOCK, 0u) : -1;
}
static inline int ipo_sem_timedwait(ipo_sem_t *semaphore, uint32_t timeout_ms) {
    return semaphore ? ipo_sync_call(semaphore, IPO_SYNC_SEMAPHORE,
                                     IPO_SYNC_TIMED_WAIT, timeout_ms) : -1;
}
static inline int ipo_sem_post(ipo_sem_t *semaphore) {
    return semaphore ? ipo_sync_call(semaphore, IPO_SYNC_SEMAPHORE,
                                     IPO_SYNC_POST, 0u) : -1;
}

static inline int ipo_cond_init(ipo_cond_t *condition) {
    return condition ? ipo_sync_call(condition, IPO_SYNC_CONDITION,
                                     IPO_SYNC_INIT, 0u) : -1;
}
static inline int ipo_cond_destroy(ipo_cond_t *condition) {
    return condition ? ipo_sync_call(condition, IPO_SYNC_CONDITION,
                                     IPO_SYNC_DESTROY, 0u) : -1;
}
static inline int ipo_cond_wait(ipo_cond_t *condition, ipo_mutex_t *mutex) {
    return condition && mutex
        ? ipo_sync_call(condition, IPO_SYNC_CONDITION, IPO_SYNC_COND_WAIT,
                        (uint32_t)(uintptr_t)mutex)
        : -1;
}
static inline int ipo_cond_timedwait(ipo_cond_t *condition, ipo_mutex_t *mutex, uint32_t timeout_ms) {
    if (!condition || !mutex) return -1;
    ipo_cond_timedwait_req_t req = { (uint32_t)(uintptr_t)mutex, timeout_ms };
    return ipo_sync_call(condition, IPO_SYNC_CONDITION, IPO_SYNC_COND_TIMEDWAIT,
                         (uint32_t)(uintptr_t)&req);
}
static inline int ipo_cond_signal(ipo_cond_t *condition) {
    return condition ? ipo_sync_call(condition, IPO_SYNC_CONDITION,
                                     IPO_SYNC_SIGNAL, 0u) : -1;
}
static inline int ipo_cond_broadcast(ipo_cond_t *condition) {
    return condition ? ipo_sync_call(condition, IPO_SYNC_CONDITION,
                                     IPO_SYNC_BROADCAST, 0u) : -1;
}

#endif
