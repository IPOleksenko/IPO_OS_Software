#include <c++/iostream>
#include <c++/exception>
#include <c++/new>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

namespace std {
    ostream cout(stdout);
    ostream cerr(stderr);
    istream cin(stdin);

    const nothrow_t nothrow{};

    static new_handler s_new_handler = nullptr;
    static terminate_handler s_terminate_handler = nullptr;
    static pthread_mutex_t s_handler_lock = PTHREAD_MUTEX_INITIALIZER;

    new_handler set_new_handler(new_handler handler) noexcept {
        pthread_mutex_lock(&s_handler_lock);
        new_handler old = s_new_handler;
        s_new_handler = handler;
        pthread_mutex_unlock(&s_handler_lock);
        return old;
    }

    new_handler get_new_handler() noexcept {
        pthread_mutex_lock(&s_handler_lock);
        new_handler cur = s_new_handler;
        pthread_mutex_unlock(&s_handler_lock);
        return cur;
    }

    terminate_handler set_terminate(terminate_handler f) noexcept {
        pthread_mutex_lock(&s_handler_lock);
        terminate_handler old = s_terminate_handler;
        s_terminate_handler = f;
        pthread_mutex_unlock(&s_handler_lock);
        return old;
    }

    terminate_handler get_terminate() noexcept {
        pthread_mutex_lock(&s_handler_lock);
        terminate_handler cur = s_terminate_handler;
        pthread_mutex_unlock(&s_handler_lock);
        return cur;
    }

    [[noreturn]] void terminate() noexcept {
        terminate_handler fn = get_terminate();
        if (fn) {
            fn();
        }
        abort();
    }
}

static void *alloc_with_handler(size_t size, int can_throw) {
    size_t req = (size == 0u) ? 1u : size;
    while (1) {
        void *p = calloc(1u, req);
        if (p) return p;
        std::new_handler h = std::get_new_handler();
        if (!h) {
            if (can_throw) {
                throw std::bad_alloc();
            }
            return nullptr;
        }
        h();
    }
}

void *operator new(size_t size) {
    return alloc_with_handler(size, 1);
}

void *operator new[](size_t size) {
    return alloc_with_handler(size, 1);
}

void *operator new(size_t size, const std::nothrow_t&) noexcept {
    return alloc_with_handler(size, 0);
}

void *operator new[](size_t size, const std::nothrow_t&) noexcept {
    return alloc_with_handler(size, 0);
}

void operator delete(void *p) noexcept {
    free(p);
}

void operator delete[](void *p) noexcept {
    free(p);
}

void operator delete(void *p, size_t size) noexcept {
    (void)size;
    free(p);
}

void operator delete[](void *p, size_t size) noexcept {
    (void)size;
    free(p);
}

void operator delete(void *p, const std::nothrow_t&) noexcept {
    free(p);
}

void operator delete[](void *p, const std::nothrow_t&) noexcept {
    free(p);
}

extern "C" {

void __cxa_pure_virtual() {
    fputs("Pure virtual function called!\n", stderr);
    std::terminate();
}

void __cxa_deleted_virtual() {
    fputs("Deleted virtual function called!\n", stderr);
    std::terminate();
}

void *__dso_handle = (void *)&__dso_handle;

struct cxa_entry_t {
    void (*func)(void *);
    void *arg;
    void *dso_handle;
};

static pthread_mutex_t s_cxa_atexit_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cxa_entry_t *cxa_list = nullptr;
static size_t cxa_count = 0u;
static size_t cxa_cap = 0u;

int __cxa_atexit(void (*func)(void *), void *arg, void *dso_handle) {
    if (!func) return -1;
    pthread_mutex_lock(&s_cxa_atexit_lock);
    if (cxa_count >= cxa_cap) {
        size_t new_cap = (cxa_cap == 0u) ? 16u : cxa_cap * 2u;
        struct cxa_entry_t *new_list =
            (struct cxa_entry_t *)realloc(cxa_list, new_cap * sizeof(struct cxa_entry_t));
        if (!new_list) {
            pthread_mutex_unlock(&s_cxa_atexit_lock);
            return -1;
        }
        cxa_list = new_list;
        cxa_cap = new_cap;
    }
    cxa_list[cxa_count].func = func;
    cxa_list[cxa_count].arg = arg;
    cxa_list[cxa_count].dso_handle = dso_handle;
    cxa_count++;
    pthread_mutex_unlock(&s_cxa_atexit_lock);
    return 0;
}

void __cxa_finalize(void *dso_handle) {
    pthread_mutex_lock(&s_cxa_atexit_lock);
    while (1) {
        int found_idx = -1;
        for (int i = (int)cxa_count - 1; i >= 0; i--) {
            if (cxa_list[i].func != nullptr &&
                (dso_handle == nullptr || cxa_list[i].dso_handle == dso_handle)) {
                found_idx = i;
                break;
            }
        }
        if (found_idx < 0) break;
        void (*f)(void *) = cxa_list[found_idx].func;
        void *arg = cxa_list[found_idx].arg;
        cxa_list[found_idx].func = nullptr;
        pthread_mutex_unlock(&s_cxa_atexit_lock);
        f(arg);
        pthread_mutex_lock(&s_cxa_atexit_lock);
    }
    if (dso_handle == nullptr && cxa_list != nullptr) {
        free(cxa_list);
        cxa_list = nullptr;
        cxa_count = 0u;
        cxa_cap = 0u;
    }
    pthread_mutex_unlock(&s_cxa_atexit_lock);
}

static pthread_mutex_t s_guard_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_guard_cond = PTHREAD_COND_INITIALIZER;

/* Itanium ABI: byte 0 stays 0 until release so inline fast-path never skips wait; byte 1 tracks init-in-progress. */
int __cxa_guard_acquire(uint64_t *guard) {
    volatile uint8_t *flag = (volatile uint8_t *)guard;
    if (__atomic_load_n(&flag[0], __ATOMIC_ACQUIRE) != 0u) {
        return 0;
    }
    pthread_mutex_lock(&s_guard_lock);
    while (__atomic_load_n(&flag[1], __ATOMIC_RELAXED) != 0u) {
        pthread_cond_wait(&s_guard_cond, &s_guard_lock);
    }
    if (__atomic_load_n(&flag[0], __ATOMIC_RELAXED) != 0u) {
        pthread_mutex_unlock(&s_guard_lock);
        return 0;
    }
    __atomic_store_n(&flag[1], 1u, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&s_guard_lock);
    return 1;
}

void __cxa_guard_release(uint64_t *guard) {
    volatile uint8_t *flag = (volatile uint8_t *)guard;
    pthread_mutex_lock(&s_guard_lock);
    __atomic_store_n(&flag[1], 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&flag[0], 1u, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&s_guard_cond);
    pthread_mutex_unlock(&s_guard_lock);
}

void __cxa_guard_abort(uint64_t *guard) {
    volatile uint8_t *flag = (volatile uint8_t *)guard;
    pthread_mutex_lock(&s_guard_lock);
    __atomic_store_n(&flag[1], 0u, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&s_guard_cond);
    pthread_mutex_unlock(&s_guard_lock);
}

} // extern "C"
