#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/random.h>
#include <syscall.h>
#include <system/timer.h>

extern int serial_printf(const char *fmt, ...);

/* =========================================================================
 * Linux i386 syscall() & POSIX shims backed by real libc/kernel primitives
 * ========================================================================= */
struct iovec {
    void *iov_base;
    size_t iov_len;
};

#define LINUX_FUTEX_WAIT        0
#define LINUX_FUTEX_WAKE        1
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_CMD_MASK    0x7F

long syscall(long number, ...) {
    va_list ap;
    va_start(ap, number);
    long ret = -1;
    if (number == 355 /* __NR_getrandom */) {
        void *buf = va_arg(ap, void *);
        size_t len = va_arg(ap, size_t);
        unsigned int flags = va_arg(ap, unsigned int);
        ssize_t r = getrandom(buf, len, flags);
        ret = (long)r;
    } else if (number == 240 /* __NR_futex */) {
        volatile uint32_t *uaddr = va_arg(ap, volatile uint32_t *);
        int op = va_arg(ap, int) & LINUX_FUTEX_CMD_MASK;
        uint32_t val = va_arg(ap, uint32_t);
        const struct timespec *timeout = va_arg(ap, const struct timespec *);
        if (!uaddr) {
            errno = EFAULT;
            ret = -1;
        } else if (op == LINUX_FUTEX_WAIT || op == LINUX_FUTEX_WAIT_BITSET) {
            if (timeout) {
                ipo_timespec_t rel;
                rel.tv_sec = (uint64_t)(timeout->tv_sec > 0 ? timeout->tv_sec : 0);
                rel.tv_nsec = (uint64_t)(timeout->tv_nsec > 0 ? timeout->tv_nsec : 0);
                int r = ipo_futex(uaddr, IPO_FUTEX_WAIT_TS, val, (uint32_t)(uintptr_t)&rel);
                if (r < 0) {
                    errno = -r;
                    ret = -1;
                } else {
                    ret = 0;
                }
            } else {
                int r = ipo_futex(uaddr, IPO_FUTEX_WAIT, val, 0u);
                if (r < 0) {
                    errno = -r;
                    ret = -1;
                } else {
                    ret = 0;
                }
            }
        } else if (op == LINUX_FUTEX_WAKE || op == LINUX_FUTEX_WAKE_BITSET) {
            int r = ipo_futex(uaddr, IPO_FUTEX_WAKE, val, 0u);
            if (r < 0) {
                errno = -r;
                ret = -1;
            } else {
                ret = r;
            }
        } else {
            errno = ENOSYS;
            ret = -1;
        }
    } else {
        errno = ENOSYS;
        ret = -1;
    }
    va_end(ap);
    return ret;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
        if (fd == 1 || fd == 2) {
            const char *s = (const char *)iov[i].iov_base;
            for (size_t k = 0; k < iov[i].iov_len; k++) serial_printf("%c", s[k]);
            total += (ssize_t)iov[i].iov_len;
        } else {
            ssize_t w = write(fd, iov[i].iov_base, iov[i].iov_len);
            if (w < 0) return w;
            total += w;
        }
    }
    return total;
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base || iov[i].iov_len == 0) continue;
        ssize_t r = read(fd, iov[i].iov_base, iov[i].iov_len);
        if (r <= 0) return total > 0 ? total : r;
        total += r;
    }
    return total;
}

int ioctl(int fd, unsigned long req, ...) { (void)fd; (void)req; errno = ENOTTY; return -1; }
int pause(void) { ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL); errno = EINTR; return -1; }
int dirfd(void *dirp) { (void)dirp; errno = EINVAL; return -1; }
int accept4(int sockfd, void *addr, void *addrlen, int flags) { (void)sockfd; (void)addr; (void)addrlen; (void)flags; errno = ENOSYS; return -1; }
int setsockopt(int s, int l, int opt, const void *val, uint32_t len) { (void)s; (void)l; (void)opt; (void)val; (void)len; return 0; }
int shutdown(int s, int how) { (void)s; (void)how; return 0; }
int __xpg_strerror_r(int errnum, char *buf, size_t buflen) {
    return strerror_r(errnum, buf, buflen);
}
unsigned long getauxval(unsigned long type) { (void)type; return 0; }
int dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data) { (void)cb; (void)data; return 0; }
int sched_yield(void) { ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL); return 0; }
int pthread_atfork(void (*p)(void), void (*pa)(void), void (*c)(void)) { (void)p; (void)pa; (void)c; return 0; }

typedef struct {
    unsigned int size;
    unsigned int align;
    void *object;
    void *templ;
} __emutls_control;

static pthread_mutex_t s_emutls_lock = PTHREAD_MUTEX_INITIALIZER;

void *__emutls_get_address(__emutls_control *ctrl) {
    if (!ctrl) return NULL;
    void *obj = __atomic_load_n(&ctrl->object, __ATOMIC_ACQUIRE);
    if (!obj) {
        pthread_mutex_lock(&s_emutls_lock);
        obj = ctrl->object;
        if (!obj) {
            size_t sz = ctrl->size ? ctrl->size : 1u;
            size_t align = ctrl->align > 4u ? ctrl->align : 4u;
            void *p = NULL;
            if (posix_memalign(&p, align, sz) == 0 && p) {
                if (ctrl->templ) memcpy(p, ctrl->templ, sz);
                else memset(p, 0, sz);
                __atomic_store_n(&ctrl->object, p, __ATOMIC_RELEASE);
                obj = p;
            }
        }
        pthread_mutex_unlock(&s_emutls_lock);
    }
    return obj;
}

typedef struct {
    unsigned long ti_module;
    unsigned long ti_offset;
} tls_index_t;

__attribute__((regparm(1))) void *___tls_get_addr(tls_index_t *ti) {
    static uint8_t s_tls_fallback[4096];
    if (ti && ti->ti_offset < sizeof(s_tls_fallback)) return s_tls_fallback + ti->ti_offset;
    return s_tls_fallback;
}

/* =========================================================================
 * Misc libc / math helpers for Rust std
 * ========================================================================= */
int bcmp(const void *s1, const void *s2, size_t n) { return memcmp(s1, s2, n); }
float roundf(float x) { return (x >= 0.0f) ? (float)(long)(x + 0.5f) : (float)(long)(x - 0.5f); }
double fma(double x, double y, double z) { return x * y + z; }

__attribute__((weak)) int rust_eh_personality(void) { return 0; }

/* f128 compiler-rt helpers only referenced if formatted */
void __addtf3(void) {}
void __subtf3(void) {}
void __multf3(void) {}
void __divtf3(void) {}
int __eqtf2(void) { return 0; }
int __gttf2(void) { return 0; }
int __letf2(void) { return 0; }
int __lttf2(void) { return 0; }
int __unordtf2(void) { return 0; }
void __floatsitf(void) {}
void __floattitf(void) {}
void __extendhfsf2(void) {}
void __truncsfhf2(void) {}
