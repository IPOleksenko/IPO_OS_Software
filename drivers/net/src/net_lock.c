#include <net/net_lock.h>
#include <ioport.h>
#include <syscall.h>
#include <stddef.h>

extern void *process_get_current_thread(void);
extern uint32_t smp_get_current_cpu_id(void);

static volatile uintptr_t s_net_owner = 0u;
static volatile uint32_t  s_net_depth = 0u;

/**
 * Compute a unique non-zero execution token distinguishing active process
 * threads from per-CPU scheduler tick contexts.
 */
static uintptr_t net_lock_self_token(void) {
    void *th = process_get_current_thread();
    if (th != NULL) {
        return (uintptr_t)th;
    }
    return ((uintptr_t)smp_get_current_cpu_id() << 1) | 1u;
}

void net_lock_acquire(void) {
    uintptr_t me = net_lock_self_token();
    if (__atomic_load_n(&s_net_owner, __ATOMIC_RELAXED) == me) {
        s_net_depth++;
        return;
    }
    uintptr_t expected = 0u;
    while (!__atomic_compare_exchange_n(&s_net_owner, &expected, me, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 0u;
        __asm__ volatile("pause");
    }
    s_net_depth = 1u;
}

bool net_lock_trylock(void) {
    uintptr_t me = net_lock_self_token();
    if (__atomic_load_n(&s_net_owner, __ATOMIC_RELAXED) == me) {
        s_net_depth++;
        return true;
    }
    uintptr_t expected = 0u;
    if (__atomic_compare_exchange_n(&s_net_owner, &expected, me, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        s_net_depth = 1u;
        return true;
    }
    return false;
}

void net_lock_release(void) {
    uintptr_t me = net_lock_self_token();
    if (__atomic_load_n(&s_net_owner, __ATOMIC_RELAXED) != me) {
        return;
    }
    if (s_net_depth > 1u) {
        s_net_depth--;
        return;
    }
    s_net_depth = 0u;
    __atomic_store_n(&s_net_owner, 0u, __ATOMIC_RELEASE);
}

void net_lock_yield_sleep(void) {
    uintptr_t me = net_lock_self_token();
    uint32_t saved_depth = 0u;
    if (__atomic_load_n(&s_net_owner, __ATOMIC_RELAXED) == me) {
        saved_depth = s_net_depth;
        s_net_depth = 0u;
        __atomic_store_n(&s_net_owner, 0u, __ATOMIC_RELEASE);
    }

    ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0, NULL);
    io_wait();

    if (saved_depth > 0u) {
        /* Reacquire directly and restore recursion depth (thread may resume on another CPU). */
        net_lock_acquire();
        if (saved_depth > 1u) {
            s_net_depth += (saved_depth - 1u);
        }
    }
}
