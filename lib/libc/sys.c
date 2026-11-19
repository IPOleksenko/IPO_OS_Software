#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <dirent.h>

#include <syscall.h>

int ipo_syscall(uint32_t num, uint32_t argc, uint32_t *argv) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(num),
          "b"(argc),
          "c"(argv)
        : "edx", "memory", "cc"
    );
    return ret;
}

uintptr_t __stack_chk_guard = 0x595e9fbd;

void __attribute__((noreturn)) __stack_chk_fail(void) {
    fputs("*** stack smashing detected ***: terminated\n", stderr);
    abort();
}

void __attribute__((noreturn, visibility("hidden"))) __stack_chk_fail_local(void) {
    __stack_chk_fail();
}

int stat(const char *pathname, struct stat *statbuf) {
    if (!pathname || !statbuf) {
        errno = EFAULT;
        return -1;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) == 0) {
        memset(statbuf, 0, sizeof(struct stat));
        if (st.mode & IPO_INODE_TYPE_DIR) {
            statbuf->st_mode = S_IFDIR | 0777;
        } else {
            statbuf->st_mode = S_IFREG | 0777;
        }
        statbuf->st_size = (uint32_t)st.size;
        return 0;
    }
    int fd = open(pathname, O_RDONLY);
    if (fd < 0) {
        errno = ENOENT;
        return -1;
    }
    int res = fstat(fd, statbuf);
    close(fd);
    return res;
}

int fstat(int fd, struct stat *statbuf) {
    if (fd < 0 || !statbuf) {
        errno = EBADF;
        return -1;
    }

    off_t cur = lseek(fd, 0, SEEK_CUR);
    off_t size = lseek(fd, 0, SEEK_END);
    if (cur != (off_t)-1) {
        lseek(fd, cur, SEEK_SET);
    }

    memset(statbuf, 0, sizeof(struct stat));
    statbuf->st_mode = S_IFREG | 0777;
    statbuf->st_size = (size == (off_t)-1) ? 0 : (int32_t)size;
    return 0;
}

static bool is_leap_year(int64_t year) {
    int64_t m400 = year % 400;
    if (m400 < 0) m400 += 400;
    return (m400 % 4 == 0 && (m400 % 100 != 0 || m400 == 0));
}

static int get_month_days(int mon, int64_t year) {
    switch (mon) {
        case 0: case 2: case 4: case 6: case 7: case 9: case 11: return 31;
        case 3: case 5: case 8: case 10: return 30;
        case 1: return is_leap_year(year) ? 29 : 28;
        default: return 0;
    }
}

static bool query_rtc_token_i64(const char *fmt, int64_t *out_val) {
    int need = ipo_rtc_get_str(fmt, NULL, 0u);
    if (need <= 0) return false;
    char *buf = (char *)malloc((size_t)need + 1u);
    if (!buf) return false;
    int got = ipo_rtc_get_str(fmt, buf, (uint32_t)need + 1u);
    bool ok = (got > 0);
    int64_t val = 0;
    if (ok) {
        int i = 0;
        bool neg = (buf[0] == '-');
        if (neg) i = 1;
        for (; i < got && buf[i] >= '0' && buf[i] <= '9'; i++) {
            if (val > (INT64_MAX - 9) / 10) {
                val = INT64_MAX / 2;
                break;
            }
            val = val * 10 + (buf[i] - '0');
        }
        if (neg) val = -val;
    }
    free(buf);
    if (ok && out_val) *out_val = val;
    return ok;
}

time_t time(time_t *t) {
    int64_t ts = 0;
    time_t now;
    if (query_rtc_token_i64("TS", &ts)) {
        /* Shift year-0 timestamp to the 1970 origin for C library callers. */
        now = (time_t)(ts - 62167219200LL);
    } else {
        now = (time_t)ipo_time(NULL);
    }
    if (t) *t = now;
    return now;
}

clock_t clock(void) {
    return (clock_t)(ipo_time(NULL) * 1000u);
}

double difftime(time_t time1, time_t time0) {
    return (double)(time1 - time0);
}

static struct tm *get_static_tm(void) {
    static struct tm s_tm;
    return &s_tm;
}

struct tm *gmtime(const time_t *timep) {
    if (!timep) return NULL;
    int64_t t = (int64_t)*timep;
    struct tm *tm_out = get_static_tm();
    memset(tm_out, 0, sizeof(*tm_out));

    int64_t rem_sec = t % 86400LL;
    int64_t days = t / 86400LL;
    if (rem_sec < 0) {
        rem_sec += 86400LL;
        days--;
    }

    tm_out->tm_sec  = (int)(rem_sec % 60LL);
    rem_sec /= 60LL;
    tm_out->tm_min  = (int)(rem_sec % 60LL);
    tm_out->tm_hour = (int)(rem_sec / 60LL);

    int64_t wday = (days + 4LL) % 7LL;
    if (wday < 0) wday += 7LL;
    tm_out->tm_wday = (int)wday;

    /* 719528 days between year 0 (proleptic) and 1970-01-01 */
    int64_t total_days = days + 719528LL;
    int64_t q400 = total_days / 146097LL;
    int64_t rem = total_days % 146097LL;
    if (rem < 0) {
        rem += 146097LL;
        q400--;
    }

    int64_t y_off = 0;
    while (y_off < 400) {
        int yd = is_leap_year(y_off) ? 366 : 365;
        if (rem < yd) break;
        rem -= yd;
        y_off++;
    }
    int64_t year = q400 * 400LL + y_off;
    tm_out->tm_year = (int)(year - 1900LL);
    tm_out->tm_yday = (int)rem;

    int mon = 0;
    while (mon < 12) {
        int mdays = get_month_days(mon, year);
        if (rem < mdays) break;
        rem -= mdays;
        mon++;
    }
    tm_out->tm_mon = mon;
    tm_out->tm_mday = (int)rem + 1;
    return tm_out;
}

struct tm *localtime(const time_t *timep) {
    return gmtime(timep);
}

time_t mktime(struct tm *timeptr) {
    if (!timeptr) return (time_t)-1;
    int64_t year = (int64_t)timeptr->tm_year + 1900LL;
    int mon = timeptr->tm_mon;
    int day = timeptr->tm_mday;

    int64_t q400 = year / 400LL;
    int64_t y_off = year % 400LL;
    if (y_off < 0) {
        y_off += 400LL;
        q400--;
    }

    int64_t total_days = q400 * 146097LL;
    for (int64_t y = 0; y < y_off; y++) {
        total_days += is_leap_year(y) ? 366LL : 365LL;
    }
    for (int m = 0; m < mon; m++) {
        total_days += (int64_t)get_month_days(m, year);
    }
    total_days += (int64_t)(day - 1);

    int64_t days = total_days - 719528LL;
    int64_t t = days * 86400LL + (int64_t)timeptr->tm_hour * 3600LL +
                (int64_t)timeptr->tm_min * 60LL + (int64_t)timeptr->tm_sec;
    return (time_t)t;
}

static char *time_buf = NULL;
static size_t time_buf_cap = 0;

char *asctime(const struct tm *timeptr) {
    if (!timeptr) return NULL;
    int year = timeptr->tm_year + 1900;
    int mon = timeptr->tm_mon + 1;
    int mday = timeptr->tm_mday;
    int hour = timeptr->tm_hour;
    int min = timeptr->tm_min;
    int sec = timeptr->tm_sec;
    int needed = snprintf(NULL, 0, "%04d-%02d-%02d %02d:%02d:%02d\n",
                          year, mon, mday, hour, min, sec);
    if (needed < 0) return NULL;
    if ((size_t)needed + 1 > time_buf_cap) {
        char *nb = (char *)realloc(time_buf, (size_t)needed + 1);
        if (!nb) return NULL;
        time_buf = nb;
        time_buf_cap = (size_t)needed + 1;
    }
    snprintf(time_buf, time_buf_cap, "%04d-%02d-%02d %02d:%02d:%02d\n",
             year, mon, mday, hour, min, sec);
    return time_buf;
}

char *ctime(const time_t *timep) {
    return asctime(localtime(timep));
}

static size_t strftime_append_fmt(char *dst, size_t max, size_t written, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (len <= 0) return written;

    char *tmp = (char *)malloc((size_t)len + 1);
    if (!tmp) return written;
    va_start(ap, fmt);
    vsnprintf(tmp, (size_t)len + 1, fmt, ap);
    va_end(ap);

    for (int i = 0; i < len && written + 1 < max; i++) {
        dst[written++] = tmp[i];
    }
    free(tmp);
    return written;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    if (!s || max == 0 || !format) return 0;
    size_t written = 0;
    const char *f = format;
    while (*f && written + 1 < max) {
        if (*f == '%') {
            f++;
            switch (*f) {
            case 'Y':
                written = strftime_append_fmt(s, max, written, "%04d", tm ? tm->tm_year + 1900 : 2026);
                break;
            case 'y':
                written = strftime_append_fmt(s, max, written, "%02d", tm ? (tm->tm_year + 1900) % 100 : 26);
                break;
            case 'm':
                written = strftime_append_fmt(s, max, written, "%02d", tm ? tm->tm_mon + 1 : 1);
                break;
            case 'd':
                written = strftime_append_fmt(s, max, written, "%02d", tm ? tm->tm_mday : 1);
                break;
            case 'H':
                written = strftime_append_fmt(s, max, written, "%02d", tm ? tm->tm_hour : 0);
                break;
            case 'M':
                written = strftime_append_fmt(s, max, written, "%02d", tm ? tm->tm_min : 0);
                break;
            case 'S':
                written = strftime_append_fmt(s, max, written, "%02d", tm ? tm->tm_sec : 0);
                break;
            case 'F':
                written = strftime_append_fmt(s, max, written, "%04d-%02d-%02d",
                                              tm ? tm->tm_year + 1900 : 2026,
                                              tm ? tm->tm_mon + 1 : 1,
                                              tm ? tm->tm_mday : 1);
                break;
            case 'T':
                written = strftime_append_fmt(s, max, written, "%02d:%02d:%02d",
                                              tm ? tm->tm_hour : 0,
                                              tm ? tm->tm_min : 0,
                                              tm ? tm->tm_sec : 0);
                break;
            case '%':
                s[written++] = '%';
                break;
            default:
                s[written++] = '%';
                if (*f && written + 1 < max) {
                    s[written++] = *f;
                }
                break;
            }
            if (*f) f++;
        } else {
            s[written++] = *f++;
        }
    }
    s[written] = '\0';
    return written;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    (void)tz;
    if (tv) {
        int64_t ms_val = 0LL;
        if (query_rtc_token_i64("TMS", &ms_val)) {
            int64_t sec_part = (ms_val / 1000LL) - 62167219200LL;
            long usec_part = (long)((ms_val % 1000LL) * 1000LL);
            if (usec_part < 0) {
                usec_part += 1000000L;
                sec_part -= 1LL;
            }
            tv->tv_sec = sec_part;
            tv->tv_usec = usec_part;
            return 0;
        }
        static int s_init = 0;
        static uint32_t s_base_ms = 0;
        static time_t s_base_sec = 0;
        uint32_t ms = (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
        if (!s_init) {
            s_base_sec = time(NULL);
            s_base_ms = ms;
            s_init = 1;
        }
        uint32_t elapsed_ms = ms - s_base_ms;
        tv->tv_sec = s_base_sec + (time_t)(elapsed_ms / 1000u);
        tv->tv_usec = (long)((elapsed_ms % 1000u) * 1000u);
    }
    return 0;
}

static int signal_syscall_result(int result) {
    if (result >= 0) return 0;
    errno = result == -2 ? ESRCH : (result == -3 ? EPERM : EINVAL);
    return -1;
}

sighandler_t signal(int signum, sighandler_t handler) {
    struct sigaction action;
    struct sigaction old_action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    if (sigaction(signum, &action, &old_action) < 0) return SIG_ERR;
    return old_action.sa_handler;
}

int raise(int sig) {
    return kill(ipo_getpid(), sig);
}

int kill(pid_t pid, int sig) {
    uint32_t args[2] = {(uint32_t)pid, (uint32_t)sig};
    return signal_syscall_result(ipo_syscall(IPO_SYSCALL_KILL, 2u, args));
}

pid_t wait(int *wstatus) {
    return waitpid(-1, wstatus, 0);
}

pid_t waitpid(pid_t pid, int *wstatus, int options) {
    (void)options;
    if (wstatus) {
        int code = ipo_get_exit_code();
        *wstatus = (code << 8);
    }
    return (pid == -1) ? 1 : pid;
}

void *__memcpy_chk(void *dest, const void *src, size_t len, size_t destlen) {
    (void)destlen;
    return memcpy(dest, src, len);
}

void *__memmove_chk(void *dest, const void *src, size_t len, size_t destlen) {
    (void)destlen;
    return memmove(dest, src, len);
}

void *__memset_chk(void *dest, int c, size_t len, size_t destlen) {
    (void)destlen;
    return memset(dest, c, len);
}

char *__strcpy_chk(char *dest, const char *src, size_t destlen) {
    (void)destlen;
    return strcpy(dest, src);
}

char *__strncpy_chk(char *dest, const char *src, size_t len, size_t destlen) {
    (void)destlen;
    return strncpy(dest, src, len);
}

int __sprintf_chk(char *s, int flag, size_t os, const char *fmt, ...) {
    (void)flag; (void)os;
    va_list ap;
    va_start(ap, fmt);
    int res = vsprintf(s, fmt, ap);
    va_end(ap);
    return res;
}

int __snprintf_chk(char *s, size_t maxlen, int flag, size_t os, const char *fmt, ...) {
    (void)flag; (void)os;
    va_list ap;
    va_start(ap, fmt);
    int res = vsnprintf(s, maxlen, fmt, ap);
    va_end(ap);
    return res;
}

int __vsnprintf_chk(char *s, size_t maxlen, int flag, size_t os, const char *fmt, va_list ap) {
    (void)flag; (void)os;
    return vsnprintf(s, maxlen, fmt, ap);
}

int __fprintf_chk(FILE *fp, int flag, const char *fmt, ...) {
    (void)flag;
    va_list ap;
    va_start(ap, fmt);
    int res = vfprintf(fp, fmt, ap);
    va_end(ap);
    return res;
}

int __printf_chk(int flag, const char *fmt, ...) {
    (void)flag;
    va_list ap;
    va_start(ap, fmt);
    int res = vprintf(fmt, ap);
    va_end(ap);
    return res;
}

char *__strcat_chk(char *dest, const char *src, size_t destlen) {
    (void)destlen;
    return strcat(dest, src);
}

void *mempcpy(void *dest, const void *src, size_t n) {
    memcpy(dest, src, n);
    return (char *)dest + n;
}

void *__mempcpy_chk(void *dest, const void *src, size_t n, size_t destlen) {
    (void)destlen;
    return mempcpy(dest, src, n);
}

int *__errno_location(void) {
    return &errno;
}

int fstat64(int fd, struct stat *buf) {
    return fstat(fd, buf);
}

int stat64(const char *path, struct stat *buf) {
    return stat(path, buf);
}

FILE *fopen64(const char *pathname, const char *mode) {
    return fopen(pathname, mode);
}

int fseeko64(FILE *stream, off_t offset, int whence) {
    return fseek(stream, (long)offset, whence);
}

off_t ftello64(FILE *stream) {
    return (off_t)ftell(stream);
}

int getrlimit(int resource, void *rlim) {
    (void)resource;
    if (rlim) {
        uint32_t *r = (uint32_t *)rlim;
        r[0] = 8 * 1024 * 1024;
        r[1] = 8 * 1024 * 1024;
    }
    return 0;
}

int getrlimit64(int resource, void *rlim) {
    (void)resource;
    if (rlim) {
        uint64_t *r = (uint64_t *)rlim;
        r[0] = 8 * 1024 * 1024;
        r[1] = 8 * 1024 * 1024;
    }
    return 0;
}

int __isoc23_sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = vsscanf(str, fmt, ap);
    va_end(ap);
    return res;
}

char *__fgets_chk(char *s, size_t size, int n, FILE *stream) {
    (void)size;
    return fgets(s, n, stream);
}

int __vfprintf_chk(FILE *fp, int flag, const char *fmt, va_list ap) {
    (void)flag;
    return vfprintf(fp, fmt, ap);
}

int ftruncate64(int fd, off_t length) {
    if (fd < 0 || length < 0) {
        errno = (fd < 0) ? EBADF : EINVAL;
        return -1;
    }
    return 0;
}

int ftruncate(int fd, off_t length) {
    return ftruncate64(fd, length);
}

int truncate(const char *path, off_t length) {
    if (!path || length < 0) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return 0;
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    (void)dirfd; (void)flags;
    return access(pathname, mode);
}

int fileno(FILE *stream) {
    if (!stream) return -1;
    return (int)stream->fd;
}

long sysconf(int name) {
    switch (name) {
    case 30: /* _SC_PAGESIZE / _SC_PAGE_SIZE */
        return 4096;
    case 2:  /* _SC_CLK_TCK */
        return 1000;
    case 4:  /* _SC_OPEN_MAX */
        return 256;
    case 83: /* _SC_NPROCESSORS_CONF */
    case 84: /* _SC_NPROCESSORS_ONLN */
        return 1;
    case 8:  /* _SC_CHILD_MAX */
        return 32;
    default:
        if (name >= 0) return 4096;
        errno = EINVAL;
        return -1;
    }
}

char *canonicalize_file_name(const char *path) {
    if (!path) return NULL;
    return realpath(path, NULL);
}

unsigned long __isoc23_strtoul(const char *nptr, char **endptr, int base) {
    return strtoul(nptr, endptr, base);
}

long __isoc23_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)prot; (void)flags;
    if (length == 0) {
        errno = EINVAL;
        return (void *)-1;
    }
    void *ptr = malloc(length);
    if (!ptr) {
        errno = ENOMEM;
        return (void *)-1;
    }
    memset(ptr, 0, length);
    if (fd >= 0) {
        off_t cur = lseek(fd, 0, SEEK_CUR);
        if (cur != (off_t)-1) {
            lseek(fd, offset, SEEK_SET);
            ssize_t n = read(fd, ptr, length);
            lseek(fd, cur, SEEK_SET);
            if (n < 0) {
                free(ptr);
                errno = ENOENT;
                return (void *)-1;
            }
        }
    }
    return ptr;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    return mmap64(addr, length, prot, flags, fd, offset);
}

int munmap(void *addr, size_t length) {
    (void)length;
    if (!addr || addr == (void *)-1) {
        errno = EINVAL;
        return -1;
    }
    free(addr);
    return 0;
}

int mprotect(void *addr, size_t length, int prot) {
    (void)addr;
    (void)length;
    (void)prot;
    return 0;
}

off_t lseek64(int fd, off_t offset, int whence) {
    return lseek(fd, offset, whence);
}

int open64(const char *pathname, int flags, ...) {
    return open(pathname, flags);
}

int fsync(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    return 0;
}

int statvfs64(const char *path, void *buf) {
    if (!path || !buf) {
        errno = (!path) ? ENOENT : EFAULT;
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    memset(buf, 0, sizeof(uint32_t) * 16);
    uint32_t args[1] = { (uint32_t)(uintptr_t)buf };
    if (ipo_syscall(IPO_SYSCALL_FS_STATVFS, 1u, args) != 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int __stat64_time64(const char *path, void *buf) {
    return stat64(path, (struct stat *)buf);
}

typedef struct {
    int fd;
    int index;
    struct dirent ent;
    char *buf;
    int buf_len;
    int buf_pos;
} ipo_dir_stream_t;

DIR *opendir(const char *name) {
    if (!name) {
        errno = ENOENT;
        return NULL;
    }
    struct stat st;
    if (stat(name, &st) != 0) {
        errno = ENOENT;
        return NULL;
    }
    if (!(st.st_mode & S_IFDIR)) {
        errno = ENOTDIR;
        return NULL;
    }

    ipo_dir_stream_t *d = (ipo_dir_stream_t *)malloc(sizeof(ipo_dir_stream_t));
    if (!d) {
        errno = ENOMEM;
        return NULL;
    }
    memset(d, 0, sizeof(*d));
    size_t cap = 8192;
    d->buf = (char *)malloc(cap);
    if (!d->buf) {
        free(d);
        errno = ENOMEM;
        return NULL;
    }
    int count = 0;
    while (1) {
        count = ipo_list_dir(name, d->buf, (int)cap);
        if (count < 0) {
            free(d->buf);
            free(d);
            errno = ENOENT;
            return NULL;
        }
        if ((size_t)count + 260 < cap || cap >= 16u * 1024u * 1024u) {
            break;
        }
        size_t new_cap = cap * 2;
        char *nb = (char *)realloc(d->buf, new_cap);
        if (!nb) break;
        d->buf = nb;
        cap = new_cap;
    }
    d->buf_len = count;
    d->buf_pos = 0;
    d->fd = 3;
    d->index = 0;
    return (DIR *)d;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        return NULL;
    }
    ipo_dir_stream_t *d = (ipo_dir_stream_t *)dirp;
    while (d->buf_pos < d->buf_len) {
        int start = d->buf_pos;
        while (d->buf_pos < d->buf_len && d->buf[d->buf_pos] != '\n' && d->buf[d->buf_pos] != '\0') {
            d->buf_pos++;
        }

        int len = d->buf_pos - start;
        if (d->buf_pos < d->buf_len && d->buf[d->buf_pos] == '\n') {
            d->buf_pos++;
        }
        if (len <= 0) {
            continue;
        }

        memset(&d->ent, 0, sizeof(struct dirent));
        if (len > 0 && d->buf[start + len - 1] == '/') {
            d->ent.d_type = DT_DIR;
            len--;
        } else {
            d->ent.d_type = DT_REG;
        }

        if (len > 255) len = 255;
        memcpy(d->ent.d_name, d->buf + start, (size_t)len);
        d->ent.d_name[len] = '\0';
        d->ent.d_ino = (ino_t)(++d->index);
        d->ent.d_off = (off_t)d->buf_pos;
        d->ent.d_reclen = sizeof(struct dirent);

        return &d->ent;
    }
    return NULL;
}

struct dirent *readdir64(DIR *dirp) {
    return readdir(dirp);
}

void rewinddir(DIR *dirp) {
    if (!dirp) return;
    ipo_dir_stream_t *d = (ipo_dir_stream_t *)dirp;
    d->buf_pos = 0;
    d->index = 0;
}

int closedir(DIR *dirp) {
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    ipo_dir_stream_t *d = (ipo_dir_stream_t *)dirp;
    if (d->buf) {
        free(d->buf);
        d->buf = NULL;
    }
    free(d);
    return 0;
}

int mkdir(const char *pathname, mode_t mode) {
    (void)mode;
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }
    int ret = ipo_create(pathname, 0x1 /* IPO_INODE_TYPE_DIR */);
    if (ret < 0) {
        errno = (ret == -2) ? EEXIST : EIO;
        return -1;
    }
    return 0;
}

int sigemptyset(sigset_t *set) {
    if (set) *set = 0;
    return 0;
}

int sigfillset(sigset_t *set) {
    if (set) *set = ~0UL;
    return 0;
}

int sigaddset(sigset_t *set, int signum) {
    if (!set || signum < 1 || signum >= 32) {
        errno = EINVAL;
        return -1;
    }
    *set |= (1UL << (signum - 1));
    return 0;
}

int sigdelset(sigset_t *set, int signum) {
    if (!set || signum < 1 || signum >= 32) {
        errno = EINVAL;
        return -1;
    }
    *set &= ~(1UL << (signum - 1));
    return 0;
}

int sigismember(const sigset_t *set, int signum) {
    if (!set || signum < 1 || signum >= 32) {
        errno = EINVAL;
        return -1;
    }
    return (*set & (1UL << (signum - 1))) ? 1 : 0;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    uint32_t requested = set ? (uint32_t)*set : 0u;
    uint32_t old_mask = 0u;
    uint32_t args[3] = {
        (uint32_t)how,
        set ? (uint32_t)(uintptr_t)&requested : 0u,
        oldset ? (uint32_t)(uintptr_t)&old_mask : 0u
    };
    int result = signal_syscall_result(
        ipo_syscall(IPO_SYSCALL_SIGPROCMASK, 3u, args));
    if (result == 0 && oldset) *oldset = old_mask;
    return result;
}

int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact) {
    uint32_t handler = act ? (uint32_t)(uintptr_t)act->sa_handler : 0u;
    uint32_t mask = act ? (uint32_t)act->sa_mask : 0u;
    uint32_t old_handler = 0u;
    uint32_t old_mask = 0u;
    uint32_t old_flags = 0u;
    uint32_t args[8] = {
        (uint32_t)signum,
        handler,
        mask,
        oldact ? (uint32_t)(uintptr_t)&old_handler : 0u,
        oldact ? (uint32_t)(uintptr_t)&old_mask : 0u,
        act ? (uint32_t)act->sa_flags : 0u,
        oldact ? (uint32_t)(uintptr_t)&old_flags : 0u,
        act ? 1u : 0u
    };
    int result = signal_syscall_result(
        ipo_syscall(IPO_SYSCALL_SIGACTION, 8u, args));
    if (result == 0 && oldact) {
        memset(oldact, 0, sizeof(*oldact));
        oldact->sa_handler = (sighandler_t)(uintptr_t)old_handler;
        oldact->sa_mask = old_mask;
        oldact->sa_flags = (int)old_flags;
    }
    return result;
}

int sigaltstack(const stack_t *ss, stack_t *old_ss) {
    uint32_t args[2] = {
        ss ? (uint32_t)(uintptr_t)ss : 0u,
        old_ss ? (uint32_t)(uintptr_t)old_ss : 0u
    };
    return signal_syscall_result(
        ipo_syscall(IPO_SYSCALL_SIGALTSTACK, 2u, args));
}

int __clock_gettime64(int clk_id, void *tp) {
    if (!tp) {
        errno = EFAULT;
        return -1;
    }
    uint32_t ms = (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
    long long *p = (long long *)tp;
    if (clk_id == 0 || clk_id == 5) { /* CLOCK_REALTIME */
        p[0] = (long long)time(NULL);
        p[1] = (long long)((ms % 1000u) * 1000000ULL);
    } else { /* CLOCK_MONOTONIC and others */
        p[0] = (long long)(ms / 1000u);
        p[1] = (long long)((ms % 1000u) * 1000000ULL);
    }
    return 0;
}

int clock_gettime(int clk_id, void *tp) {
    if (!tp) {
        errno = EFAULT;
        return -1;
    }
    uint32_t ms = (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
    long *p = (long *)tp;
    if (clk_id == 0 || clk_id == 5) {
        p[0] = (long)time(NULL);
        p[1] = (long)((ms % 1000u) * 1000000UL);
    } else {
        p[0] = (long)(ms / 1000u);
        p[1] = (long)((ms % 1000u) * 1000000UL);
    }
    return 0;
}

int __gettimeofday64(struct timeval *tv, void *tz) {
    (void)tz;
    return gettimeofday(tv, NULL);
}

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    if ((!buf && buflen != 0u) || buflen > INT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (buflen == 0u) return 0;

    static int has_rdrand = -1;
    if (has_rdrand < 0) {
        uint32_t eax = 1u, ebx, ecx, edx;
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1u), "c"(0u));
        has_rdrand = (ecx & (1u << 30)) != 0u;
    }
    if (!has_rdrand) {
        errno = ENOSYS;
        return -1;
    }

    uint8_t *output = (uint8_t *)buf;
    size_t remaining = buflen;
    while (remaining > 0u) {
        uint32_t word;
        uint8_t ready = 0;
        for (unsigned attempt = 0; attempt < 10u && !ready; attempt++) {
            __asm__ volatile("rdrand %0; setc %1"
                             : "=r"(word), "=qm"(ready));
        }
        if (!ready) {
            errno = EIO;
            return -1;
        }
        for (unsigned byte = 0; byte < 4u && remaining > 0u; byte++, remaining--) {
            *output++ = (uint8_t)(word >> (byte * 8u));
        }
    }
    return (ssize_t)buflen;
}

long long __time64(void *tloc) {
    long long t = (long long)time(NULL);
    if (tloc) *(long long *)tloc = t;
    return t;
}

struct tm *__localtime64(const void *timer) {
    time_t t = timer ? (time_t)*(const long long *)timer : time(NULL);
    return localtime(&t);
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (!result) return NULL;
    struct tm *t = gmtime(timep);
    if (!t) return NULL;
    *result = *t;
    return result;
}

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    if (!result) return NULL;
    struct tm *t = localtime(timep);
    if (!t) return NULL;
    *result = *t;
    return result;
}

int mkstemp(char *tmpl) {
    if (!tmpl) {
        errno = EINVAL;
        return -1;
    }
    return open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
}
