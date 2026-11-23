#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

void *operator new(size_t sz) { return calloc(1, sz ? sz : 1); }
void *operator new[](size_t sz) { return calloc(1, sz ? sz : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" {

void *__dso_handle = (void *)&__dso_handle;

void __cxa_pure_virtual(void) {
    abort();
}

int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
    (void)func;
    (void)arg;
    (void)dso;
    return 0;
}

int __cxa_guard_acquire(uint64_t *guard) {
    volatile uint8_t *p = (volatile uint8_t *)guard;
    return *p == 0 ? 1 : 0;
}

void __cxa_guard_release(uint64_t *guard) {
    volatile uint8_t *p = (volatile uint8_t *)guard;
    *p = 1;
}

void __cxa_guard_abort(uint64_t *guard) {
    (void)guard;
}

}
