#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/mman.h>
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
#include <pthread.h>
#include <limits.h>

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

extern const char *ipo_libc_get_fd_path(int fd);
extern int ipo_libc_parent_dir_errno(const char *pathname);
extern int ipo_libc_fd_fstat(int fd, struct stat *statbuf);
extern int ipo_libc_fd_get_kfd(int fd, int require_write);
extern int ipo_libc_fd_is_open(int fd);

#ifndef IPO_INODE_FLAG_PROTECTED
#define IPO_INODE_FLAG_PROTECTED 0x80000000u
#endif

int stat(const char *pathname, struct stat *statbuf) {
    if (!pathname || !statbuf) {
        errno = EFAULT;
        return -1;
    }
    if (pathname[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (strcmp(pathname, "/dev/null") == 0) {
        memset(statbuf, 0, sizeof(struct stat));
        statbuf->st_mode = S_IFCHR | 0666;
        statbuf->st_nlink = 1;
        return 0;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) == 0) {
        size_t plen = strlen(pathname);
        if (plen > 1 && pathname[plen - 1] == '/' && !(st.mode & IPO_INODE_TYPE_DIR)) {
            errno = ENOTDIR;
            return -1;
        }
        memset(statbuf, 0, sizeof(struct stat));
        if (st.mode & IPO_INODE_TYPE_DIR) {
            statbuf->st_mode = S_IFDIR | 0755;
        } else if ((st.mode & IPO_INODE_FLAG_PROTECTED) ||
                   strncmp(pathname, "/applications/", 14) == 0) {
            statbuf->st_mode = S_IFREG | 0755;
        } else {
            statbuf->st_mode = S_IFREG | 0644;
        }
        statbuf->st_nlink = 1;
        statbuf->st_size = (uint32_t)st.size;
        return 0;
    }
    errno = ipo_libc_parent_dir_errno(pathname);
    return -1;
}

int lstat(const char *pathname, struct stat *statbuf) {
    return stat(pathname, statbuf);
}

int fstat(int fd, struct stat *statbuf) {
    return ipo_libc_fd_fstat(fd, statbuf);
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

static pthread_once_t s_tm_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t s_tm_key;
static int s_tm_key_ok = 0;

static void init_tm_key(void) {
    if (pthread_key_create(&s_tm_key, free) == 0) {
        s_tm_key_ok = 1;
    }
}

static struct tm *get_tls_tm(void) {
    static struct tm s_fallback_tm;
    pthread_once(&s_tm_key_once, init_tm_key);
    if (!s_tm_key_ok) {
        return &s_fallback_tm;
    }
    struct tm *t = (struct tm *)pthread_getspecific(s_tm_key);
    if (!t) {
        t = (struct tm *)calloc(1, sizeof(struct tm));
        if (!t) return &s_fallback_tm;
        if (pthread_setspecific(s_tm_key, t) != 0) {
            free(t);
            return &s_fallback_tm;
        }
    }
    return t;
}

struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    if (!timep || !result) {
        errno = EINVAL;
        return NULL;
    }
    int64_t t = (int64_t)*timep;
    memset(result, 0, sizeof(*result));

    int64_t rem_sec = t % 86400LL;
    int64_t days = t / 86400LL;
    if (rem_sec < 0) {
        rem_sec += 86400LL;
        days--;
    }

    result->tm_sec  = (int)(rem_sec % 60LL);
    rem_sec /= 60LL;
    result->tm_min  = (int)(rem_sec % 60LL);
    result->tm_hour = (int)(rem_sec / 60LL);

    int64_t wday = (days + 4LL) % 7LL;
    if (wday < 0) wday += 7LL;
    result->tm_wday = (int)wday;

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
    result->tm_year = (int)(year - 1900LL);
    result->tm_yday = (int)rem;

    int mon = 0;
    while (mon < 12) {
        int mdays = get_month_days(mon, year);
        if (rem < mdays) break;
        rem -= mdays;
        mon++;
    }
    result->tm_mon = mon;
    result->tm_mday = (int)rem + 1;
    return result;
}

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    return gmtime_r(timep, result);
}

struct tm *gmtime(const time_t *timep) {
    return gmtime_r(timep, get_tls_tm());
}

struct tm *localtime(const time_t *timep) {
    return localtime_r(timep, get_tls_tm());
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

static pthread_once_t s_asctime_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t s_asctime_key;
static int s_asctime_key_ok = 0;

static void init_asctime_key(void) {
    if (pthread_key_create(&s_asctime_key, free) == 0) {
        s_asctime_key_ok = 1;
    }
}

static char *get_tls_asctime_buf(void) {
    static char s_fallback_buf[32];
    pthread_once(&s_asctime_key_once, init_asctime_key);
    if (!s_asctime_key_ok) return s_fallback_buf;
    char *b = (char *)pthread_getspecific(s_asctime_key);
    if (!b) {
        b = (char *)malloc(32u);
        if (!b) return s_fallback_buf;
        if (pthread_setspecific(s_asctime_key, b) != 0) {
            free(b);
            return s_fallback_buf;
        }
    }
    return b;
}

char *asctime_r(const struct tm *timeptr, char *buf) {
    if (!timeptr || !buf) {
        errno = EINVAL;
        return NULL;
    }
    int year = timeptr->tm_year + 1900;
    int mon = timeptr->tm_mon + 1;
    int mday = timeptr->tm_mday;
    int hour = timeptr->tm_hour;
    int min = timeptr->tm_min;
    int sec = timeptr->tm_sec;
    snprintf(buf, 26u, "%04d-%02d-%02d %02d:%02d:%02d\n",
             year, mon, mday, hour, min, sec);
    return buf;
}

char *ctime_r(const time_t *timep, char *buf) {
    struct tm tm_val;
    if (!timep || !buf || !localtime_r(timep, &tm_val)) {
        return NULL;
    }
    return asctime_r(&tm_val, buf);
}

char *asctime(const struct tm *timeptr) {
    return asctime_r(timeptr, get_tls_asctime_buf());
}

char *ctime(const time_t *timep) {
    struct tm tm_val;
    if (!timep || !localtime_r(timep, &tm_val)) {
        return NULL;
    }
    return asctime(&tm_val);
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
    static const char *const wday_abbr[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const wday_full[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const mon_abbr[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static const char *const mon_full[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    if (!s || max == 0 || !format) return 0;
    size_t written = 0;
    const char *f = format;
    while (*f && written + 1 < max) {
        if (*f == '%') {
            f++;
            switch (*f) {
            case 'a':
                written = strftime_append_fmt(s, max, written, "%s",
                                              wday_abbr[(tm && tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0]);
                break;
            case 'A':
                written = strftime_append_fmt(s, max, written, "%s",
                                              wday_full[(tm && tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0]);
                break;
            case 'b':
            case 'h':
                written = strftime_append_fmt(s, max, written, "%s",
                                              mon_abbr[(tm && tm->tm_mon >= 0 && tm->tm_mon < 12) ? tm->tm_mon : 0]);
                break;
            case 'B':
                written = strftime_append_fmt(s, max, written, "%s",
                                              mon_full[(tm && tm->tm_mon >= 0 && tm->tm_mon < 12) ? tm->tm_mon : 0]);
                break;
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
            case 'e':
                written = strftime_append_fmt(s, max, written, "%2d", tm ? tm->tm_mday : 1);
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
            case 'R':
                written = strftime_append_fmt(s, max, written, "%02d:%02d",
                                              tm ? tm->tm_hour : 0,
                                              tm ? tm->tm_min : 0);
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
            case 'c':
                written = strftime_append_fmt(s, max, written, "%s %s %2d %02d:%02d:%02d %04d",
                                              wday_abbr[(tm && tm->tm_wday >= 0 && tm->tm_wday < 7) ? tm->tm_wday : 0],
                                              mon_abbr[(tm && tm->tm_mon >= 0 && tm->tm_mon < 12) ? tm->tm_mon : 0],
                                              tm ? tm->tm_mday : 1,
                                              tm ? tm->tm_hour : 0,
                                              tm ? tm->tm_min : 0,
                                              tm ? tm->tm_sec : 0,
                                              tm ? tm->tm_year + 1900 : 2026);
                break;
            case 'z':
                written = strftime_append_fmt(s, max, written, "+0000");
                break;
            case 'Z':
                written = strftime_append_fmt(s, max, written, "UTC");
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
    if ((options & ~(WNOHANG | WUNTRACED)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (pid < -1) {
        errno = ECHILD;
        return -1;
    }
    if (wstatus) {
        int code = ipo_get_exit_code();
        *wstatus = ((code & 0xff) << 8);
    }
    return (pid <= 0) ? 1 : pid;
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

/* __errno_location is provided per-thread in lib/libc/tls_pthread.c */

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

int truncate(const char *path, off_t length) {
    if (!path || length < 0) {
        errno = EINVAL;
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }
    if (length == (off_t)st.st_size) {
        return 0;
    }
    if (ipo_truncate64(path, (uint64_t)length) < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int ftruncate64(int fd, off_t length) {
    if (length < 0) {
        errno = EINVAL;
        return -1;
    }
    int kfd = ipo_libc_fd_get_kfd(fd, 1);
    if (kfd < 0) {
        return -1;
    }
    if (ipo_ftruncate64(kfd, (uint64_t)length) < 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int ftruncate(int fd, off_t length) {
    return ftruncate64(fd, length);
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
    case 84: /* _SC_NPROCESSORS_ONLN */ {
        int ncpu = ipo_syscall(IPO_SYSCALL_SMP_GET_COUNT, 0u, NULL);
        return (ncpu > 0) ? (long)ncpu : 1L;
    }
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

typedef struct {
    uintptr_t base;
    size_t len;
} mmap_rec_t;

static pthread_mutex_t s_mmap_lock = PTHREAD_MUTEX_INITIALIZER;
static mmap_rec_t *s_mmap_recs = NULL;
static size_t s_mmap_count = 0;
static size_t s_mmap_cap = 0;

static void mmap_track_add(uintptr_t base, size_t len) {
    pthread_mutex_lock(&s_mmap_lock);
    if (s_mmap_count == s_mmap_cap) {
        size_t ncap = s_mmap_cap ? (s_mmap_cap * 2u) : 16u;
        mmap_rec_t *nr = (mmap_rec_t *)realloc(s_mmap_recs, ncap * sizeof(mmap_rec_t));
        if (!nr) {
            pthread_mutex_unlock(&s_mmap_lock);
            return;
        }
        s_mmap_recs = nr;
        s_mmap_cap = ncap;
    }
    s_mmap_recs[s_mmap_count].base = base;
    s_mmap_recs[s_mmap_count].len = len;
    s_mmap_count++;
    pthread_mutex_unlock(&s_mmap_lock);
}

static void mmap_track_remove(uintptr_t base, size_t len) {
    uintptr_t end = base + len;
    pthread_mutex_lock(&s_mmap_lock);
    for (size_t i = 0; i < s_mmap_count; i++) {
        uintptr_t rbase = s_mmap_recs[i].base;
        uintptr_t rend = rbase + s_mmap_recs[i].len;
        if (base <= rbase && end >= rend) {
            s_mmap_recs[i] = s_mmap_recs[--s_mmap_count];
            i--;
        } else if (base > rbase && end < rend) {
            /* Split middle unmap into prefix and suffix if space allows */
            s_mmap_recs[i].len = base - rbase;
            if (s_mmap_count < s_mmap_cap) {
                s_mmap_recs[s_mmap_count].base = end;
                s_mmap_recs[s_mmap_count].len = rend - end;
                s_mmap_count++;
            } else {
                size_t ncap = s_mmap_cap * 2u;
                mmap_rec_t *nr = (mmap_rec_t *)realloc(s_mmap_recs, ncap * sizeof(mmap_rec_t));
                if (nr) {
                    s_mmap_recs = nr;
                    s_mmap_cap = ncap;
                    s_mmap_recs[s_mmap_count].base = end;
                    s_mmap_recs[s_mmap_count].len = rend - end;
                    s_mmap_count++;
                }
            }
        } else if (base > rbase && base < rend) {
            s_mmap_recs[i].len = base - rbase;
        } else if (end > rbase && end < rend) {
            s_mmap_recs[i].base = end;
            s_mmap_recs[i].len = rend - end;
        }
    }
    pthread_mutex_unlock(&s_mmap_lock);
}

void *mmap64(void *addr, size_t length, int prot, int flags, int fd, uint64_t offset) {
    if (length == 0 || (offset & 0xFFFu) != 0ULL) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    if (length > 0xFFFFF000u) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    size_t aligned_len = (length + 4095u) & ~4095u;
    void *hint = addr;
    if ((uintptr_t)hint & 0xFFFu) {
        if (flags & MAP_FIXED) {
            errno = EINVAL;
            return MAP_FAILED;
        }
        hint = (void *)((uintptr_t)hint & ~0xFFFu);
    }

    uint32_t kflags = (uint32_t)flags;
    if (fd < 0) kflags |= IPO_MAP_ANONYMOUS;
    int need_populate = (fd >= 0 && !(kflags & IPO_MAP_ANONYMOUS));
    uint32_t init_prot = (uint32_t)prot;
    if (need_populate) init_prot |= IPO_PROT_WRITE | IPO_PROT_READ;

    void *ptr = ipo_mmap(hint, (uint32_t)aligned_len, init_prot, kflags);
    if (!ptr || ptr == MAP_FAILED) {
        errno = ENOMEM;
        return MAP_FAILED;
    }

    if (need_populate) {
        uint8_t *dst = (uint8_t *)ptr;
        size_t done = 0;
        while (done < length) {
            ssize_t n = pread(fd, dst + done, length - done, (off_t)(offset + (uint64_t)done));
            if (n < 0) {
                int saved = errno;
                ipo_munmap(ptr, (uint32_t)aligned_len);
                errno = saved ? saved : EIO;
                return MAP_FAILED;
            }
            if (n == 0) break;
            done += (size_t)n;
        }
        if (init_prot != (uint32_t)prot) {
            if (ipo_mprotect(ptr, (uint32_t)aligned_len, (uint32_t)prot) < 0) {
                ipo_munmap(ptr, (uint32_t)aligned_len);
                errno = EINVAL;
                return MAP_FAILED;
            }
        }
    }

    mmap_track_add((uintptr_t)ptr, aligned_len);
    return ptr;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    if (offset < 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    return mmap64(addr, length, prot, flags, fd, (uint64_t)(uint32_t)offset);
}

int munmap(void *addr, size_t length) {
    if (!addr || addr == MAP_FAILED || length == 0 || ((uintptr_t)addr & 0xFFFu) != 0) {
        errno = EINVAL;
        return -1;
    }
    size_t aligned_len = (length + 4095u) & ~4095u;
    if (aligned_len < length) {
        errno = EINVAL;
        return -1;
    }
    if (ipo_munmap(addr, (uint32_t)aligned_len) < 0) {
        errno = EINVAL;
        return -1;
    }
    mmap_track_remove((uintptr_t)addr, aligned_len);
    return 0;
}

int mprotect(void *addr, size_t length, int prot) {
    if (!addr || ((uintptr_t)addr & 0xFFFu) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (length == 0) return 0;
    size_t aligned_len = (length + 4095u) & ~4095u;
    if (aligned_len < length || (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (ipo_mprotect(addr, (uint32_t)aligned_len, (uint32_t)prot) < 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

off_t lseek64(int fd, off_t offset, int whence) {
    return lseek(fd, offset, whence);
}

int open64(const char *pathname, int flags, ...) {
    return open(pathname, flags);
}

int fsync(int fd) {
    if (!ipo_libc_fd_is_open(fd)) {
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
    if (pathname[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    int perr = ipo_libc_parent_dir_errno(pathname);
    if (perr == ENOTDIR) {
        errno = ENOTDIR;
        return -1;
    }
    int ret = ipo_mkdir(pathname);
    if (ret < 0) {
        errno = ipo_libc_parent_dir_errno(pathname);
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
    ipo_timespec_t kts;
    uint32_t kclk = (clk_id == CLOCK_REALTIME || clk_id == 5) ? IPO_CLOCK_REALTIME : IPO_CLOCK_MONOTONIC;
    if (ipo_clock_gettime(kclk, &kts) < 0) {
        errno = EINVAL;
        return -1;
    }
    long long *p = (long long *)tp;
    p[0] = (long long)kts.tv_sec;
    p[1] = (long long)kts.tv_nsec;
    return 0;
}

int clock_gettime(int clk_id, struct timespec *tp) {
    if (!tp) {
        errno = EFAULT;
        return -1;
    }
    ipo_timespec_t kts;
    uint32_t kclk = (clk_id == CLOCK_REALTIME || clk_id == 5) ? IPO_CLOCK_REALTIME : IPO_CLOCK_MONOTONIC;
    if (ipo_clock_gettime(kclk, &kts) < 0) {
        errno = EINVAL;
        return -1;
    }
    tp->tv_sec = (long)kts.tv_sec;
    tp->tv_nsec = (long)kts.tv_nsec;
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req) {
        errno = EFAULT;
        return -1;
    }
    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    ipo_timespec_t kreq;
    ipo_timespec_t krem;
    kreq.tv_sec = (uint64_t)req->tv_sec;
    kreq.tv_nsec = (uint64_t)req->tv_nsec;
    krem.tv_sec = 0ULL;
    krem.tv_nsec = 0ULL;
    int r = ipo_nanosleep(&kreq, rem ? &krem : NULL);
    if (r < 0) {
        if (rem) {
            rem->tv_sec = (long)krem.tv_sec;
            rem->tv_nsec = (long)krem.tv_nsec;
        }
        errno = (r < -1) ? -r : EINTR;
        return -1;
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

int __gettimeofday64(struct timeval *tv, void *tz) {
    (void)tz;
    return gettimeofday(tv, NULL);
}

static pthread_mutex_t s_prng_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_prng_state = 0;

static uint64_t next_prng_u64(void) {
    pthread_mutex_lock(&s_prng_lock);
    if (s_prng_state == 0) {
        uint32_t lo = 0, hi = 0;
#ifdef __i386__
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
#endif
        uint64_t tsc = ((uint64_t)hi << 32) | lo;
        uint64_t ms = (uint64_t)(uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
        uint64_t pid = (uint64_t)(uint32_t)ipo_getpid();
        uint64_t now = (uint64_t)time(NULL);
        s_prng_state = tsc ^ (ms << 20) ^ (pid << 40) ^ (now * 0x9E3779B97F4A7C15ULL) ^ 0xA5A55A5AF0F00F0FULL;
        if (s_prng_state == 0) s_prng_state = 0xCAFEBABE12345678ULL;
    }
    uint64_t x = s_prng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s_prng_state = x;
    uint64_t out = x * 0x2545F4914F6CDD1DULL;
    pthread_mutex_unlock(&s_prng_lock);
    return out;
}

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    if ((!buf && buflen != 0u) || buflen > INT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (buflen == 0u) return 0;

    static int s_has_rdrand = -1;
    int rdrand_ok = __atomic_load_n(&s_has_rdrand, __ATOMIC_ACQUIRE);
    if (rdrand_ok < 0) {
        uint32_t eax = 1u, ebx, ecx, edx;
        __asm__ volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1u), "c"(0u));
        rdrand_ok = ((ecx & (1u << 30)) != 0u) ? 1 : 0;
        __atomic_store_n(&s_has_rdrand, rdrand_ok, __ATOMIC_RELEASE);
    }

    uint8_t *output = (uint8_t *)buf;
    size_t remaining = buflen;
    while (remaining > 0u) {
        uint32_t word = 0;
        uint8_t ready = 0;
        if (rdrand_ok) {
            for (unsigned attempt = 0; attempt < 10u && !ready; attempt++) {
                __asm__ volatile("rdrand %0; setc %1"
                                 : "=r"(word), "=qm"(ready));
            }
        }
        if (!ready) {
            word = (uint32_t)next_prng_u64();
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

int mkstemp(char *tmpl) {
    if (!tmpl) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(tmpl);
    if (len >= 6 && strcmp(tmpl + len - 6, "XXXXXX") == 0) {
        static const char letters[] =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        char *xes = tmpl + len - 6;
        for (int tries = 0; tries < 1024; tries++) {
            uint64_t r = next_prng_u64();
            for (int i = 0; i < 6; i++) {
                xes[i] = letters[r % 62];
                r /= 62;
            }
            int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (fd >= 0) return fd;
            if (errno != EEXIST) return -1;
        }
        errno = EEXIST;
        return -1;
    }
    return open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
}
