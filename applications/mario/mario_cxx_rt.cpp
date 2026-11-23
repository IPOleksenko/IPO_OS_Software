#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

extern void *malloc(size_t size);
extern void free(void *ptr);

void *operator new(size_t sz) { return malloc(sz ? sz : 1); }
void *operator new[](size_t sz) { return malloc(sz ? sz : 1); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" {
void *__dso_handle = nullptr;
int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
    (void)func; (void)arg; (void)dso;
    return 0;
}
void __cxa_pure_virtual(void) {
    for (;;) {}
}
}

