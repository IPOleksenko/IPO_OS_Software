#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <math.h>
#include <pthread.h>
#include <syscall.h>
#include "ipo_libc_internal.h"

static FILE **file_pool = NULL;
static size_t file_pool_count = 0;
static size_t file_pool_cap = 0;
static pthread_mutex_t s_file_pool_lock = PTHREAD_MUTEX_INITIALIZER;

static FILE std_streams[3] = {
    { .fd = 0, .eof = 0, .error = 0, .ungotten = 0, .has_ungot = 0, .buf_mode = _IONBF },
    { .fd = 1, .eof = 0, .error = 0, .ungotten = 0, .has_ungot = 0, .buf_mode = _IONBF },
    { .fd = 2, .eof = 0, .error = 0, .ungotten = 0, .has_ungot = 0, .buf_mode = _IONBF }
};

FILE *stdin = &std_streams[0];
FILE *stdout = &std_streams[1];
FILE *stderr = &std_streams[2];

void flockfile(FILE *stream) {
    if (!stream) return;
    uint32_t self = (uint32_t)pthread_self();
    if (__atomic_load_n(&stream->lock_state, __ATOMIC_RELAXED) != 0u &&
        __atomic_load_n(&stream->lock_owner, __ATOMIC_RELAXED) == self) {
        stream->lock_depth++;
        return;
    }
    ipo_libc_raw_futex_lock(&stream->lock_state);
    __atomic_store_n(&stream->lock_owner, self, __ATOMIC_RELAXED);
    stream->lock_depth = 1u;
}

int ftrylockfile(FILE *stream) {
    if (!stream) return -1;
    uint32_t self = (uint32_t)pthread_self();
    if (__atomic_load_n(&stream->lock_state, __ATOMIC_RELAXED) != 0u &&
        __atomic_load_n(&stream->lock_owner, __ATOMIC_RELAXED) == self) {
        stream->lock_depth++;
        return 0;
    }
    uint32_t expected = 0u;
    if (__atomic_compare_exchange_n(&stream->lock_state, &expected, 1u, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_store_n(&stream->lock_owner, self, __ATOMIC_RELAXED);
        stream->lock_depth = 1u;
        return 0;
    }
    return -1;
}

void funlockfile(FILE *stream) {
    if (!stream) return;
    if (stream->lock_depth > 1u) {
        stream->lock_depth--;
        return;
    }
    __atomic_store_n(&stream->lock_owner, 0u, __ATOMIC_RELAXED);
    stream->lock_depth = 0u;
    ipo_libc_raw_futex_unlock(&stream->lock_state);
}

static int flush_stream_locked(FILE *stream) {
    if (!stream) return 0;
    if (stream->buf_writing && stream->buf && stream->buf_pos > 0u) {
        size_t sent = 0u;
        while (sent < stream->buf_pos) {
            ssize_t n = write(stream->fd, stream->buf + sent, stream->buf_pos - sent);
            if (n <= 0) {
                stream->error = 1;
                return EOF;
            }
            sent += (size_t)n;
        }
        stream->buf_pos = 0u;
    } else if (!stream->buf_writing && stream->buf && stream->buf_pos < stream->buf_end) {
        size_t unread = stream->buf_end - stream->buf_pos;
        if (unread > 0u) {
            lseek(stream->fd, -(off_t)unread, SEEK_CUR);
        }
        stream->buf_pos = 0u;
        stream->buf_end = 0u;
    }
    return 0;
}

static void free_stream_buffers_locked(FILE *stream) {
    if (stream->buf_owned && stream->buf) {
        free(stream->buf);
    }
    stream->buf = NULL;
    stream->buf_cap = 0u;
    stream->buf_pos = 0u;
    stream->buf_end = 0u;
    stream->buf_owned = 0;
    stream->buf_writing = 0;
    if (stream->tmp_unlink_path) {
        free(stream->tmp_unlink_path);
        stream->tmp_unlink_path = NULL;
    }
}

static int parse_fopen_flags(const char *mode) {
    int flags = O_RDONLY;
    if (strchr(mode, 'w')) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (strchr(mode, '+')) flags = O_RDWR | O_CREAT | O_TRUNC;
    } else if (strchr(mode, 'a')) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        if (strchr(mode, '+')) flags = O_RDWR | O_CREAT | O_APPEND;
    } else if (strchr(mode, '+')) {
        flags = O_RDWR;
    }
    return flags;
}

FILE *fopen(const char *pathname, const char *mode) {
    if (!pathname || !mode) {
        errno = EINVAL;
        return NULL;
    }

    int flags = parse_fopen_flags(mode);
    int fd = open(pathname, flags, 0666);
    if (fd < 0) return NULL;

    FILE *f = fdopen(fd, mode);
    if (!f) {
        close(fd);
        return NULL;
    }
    return f;
}

FILE *fdopen(int fd, const char *mode) {
    (void)mode;
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }

    FILE *f = (FILE *)calloc(1, sizeof(FILE));
    if (!f) {
        errno = ENOMEM;
        return NULL;
    }
    f->fd = fd;
    f->buf_mode = _IONBF;

    pthread_mutex_lock(&s_file_pool_lock);
    if (file_pool_count >= file_pool_cap) {
        size_t new_cap = (file_pool_cap == 0) ? 16 : file_pool_cap * 2;
        FILE **new_pool = (FILE **)realloc(file_pool, new_cap * sizeof(FILE *));
        if (!new_pool) {
            pthread_mutex_unlock(&s_file_pool_lock);
            free(f);
            errno = ENOMEM;
            return NULL;
        }
        file_pool = new_pool;
        file_pool_cap = new_cap;
    }

    file_pool[file_pool_count++] = f;
    pthread_mutex_unlock(&s_file_pool_lock);
    return f;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
    if (!pathname || !mode) {
        errno = EINVAL;
        return NULL;
    }
    if (!stream) {
        return fopen(pathname, mode);
    }

    int new_fd = open(pathname, parse_fopen_flags(mode), 0666);
    if (new_fd < 0) return NULL;

    flockfile(stream);
    flush_stream_locked(stream);
    if (stream == stdin || stream == stdout || stream == stderr) {
        int fixed_fd = (stream == stdin) ? 0 : (stream == stdout ? 1 : 2);
        if (new_fd != fixed_fd) {
            dup2(new_fd, fixed_fd);
            close(new_fd);
        }
        stream->fd = fixed_fd;
    } else {
        if (stream->fd >= 0) close(stream->fd);
        stream->fd = new_fd;
    }
    stream->eof = 0;
    stream->error = 0;
    stream->has_ungot = 0;
    stream->ungotten = 0;
    stream->buf_pos = 0u;
    stream->buf_end = 0u;
    stream->buf_writing = 0;
    if (stream->tmp_unlink_path) {
        unlink(stream->tmp_unlink_path);
        free(stream->tmp_unlink_path);
        stream->tmp_unlink_path = NULL;
    }
    funlockfile(stream);
    return stream;
}

int fclose(FILE *stream) {
    if (!stream) {
        errno = EBADF;
        return EOF;
    }

    flockfile(stream);
    int flush_rc = flush_stream_locked(stream);
    int res = (stream->fd >= 0) ? close(stream->fd) : -1;
    stream->fd = -1;
    char *unlink_path = stream->tmp_unlink_path;
    stream->tmp_unlink_path = NULL;
    free_stream_buffers_locked(stream);
    funlockfile(stream);

    if (unlink_path) {
        unlink(unlink_path);
        free(unlink_path);
    }

    int is_std = (stream == stdin || stream == stdout || stream == stderr);
    if (!is_std) {
        pthread_mutex_lock(&s_file_pool_lock);
        for (size_t i = 0; i < file_pool_count; i++) {
            if (file_pool[i] == stream) {
                file_pool[i] = file_pool[file_pool_count - 1];
                file_pool_count--;
                break;
            }
        }
        pthread_mutex_unlock(&s_file_pool_lock);
        free(stream);
    }
    return (res < 0 || flush_rc != 0) ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    size_t total = size * nmemb;
    size_t bytes_read = 0;
    char *out = (char *)ptr;

    flockfile(stream);
    if (stream->buf_writing) flush_stream_locked(stream);
    if (stream->has_ungot) {
        *out++ = (char)stream->ungotten;
        stream->has_ungot = 0;
        bytes_read++;
    }

    while (bytes_read < total) {
        ssize_t n = read(stream->fd, out, total - bytes_read);
        if (n < 0) {
            stream->error = 1;
            break;
        }
        if (n == 0) {
            stream->eof = 1;
            break;
        }
        bytes_read += (size_t)n;
        out += n;
    }
    funlockfile(stream);

    return bytes_read / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    size_t total = size * nmemb;
    size_t bytes_written = 0;
    const char *in = (const char *)ptr;

    flockfile(stream);
    if (stream->buf && stream->buf_cap > 0u && stream->buf_mode != _IONBF) {
        stream->buf_writing = 1;
        while (bytes_written < total) {
            if (stream->buf_pos == stream->buf_cap) {
                if (flush_stream_locked(stream) != 0) break;
            }
            if (total - bytes_written >= stream->buf_cap && stream->buf_pos == 0u) {
                ssize_t n = write(stream->fd, in + bytes_written, total - bytes_written);
                if (n <= 0) { stream->error = 1; break; }
                bytes_written += (size_t)n;
                continue;
            }
            size_t avail = stream->buf_cap - stream->buf_pos;
            size_t chunk = (total - bytes_written < avail) ? (total - bytes_written) : avail;
            memcpy(stream->buf + stream->buf_pos, in + bytes_written, chunk);
            stream->buf_pos += chunk;
            bytes_written += chunk;
            if (stream->buf_mode == _IOLBF && memchr(stream->buf + stream->buf_pos - chunk, '\n', chunk) != NULL) {
                if (flush_stream_locked(stream) != 0) break;
            }
        }
        funlockfile(stream);
        return bytes_written / size;
    }

    while (bytes_written < total) {
        ssize_t n = write(stream->fd, in, total - bytes_written);
        if (n <= 0) {
            stream->error = 1;
            break;
        }
        bytes_written += (size_t)n;
        in += n;
    }
    funlockfile(stream);

    return bytes_written / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) {
        errno = EBADF;
        return -1;
    }
    flockfile(stream);
    flush_stream_locked(stream);
    if (whence == SEEK_CUR && stream->has_ungot) {
        offset--;
    }
    stream->has_ungot = 0;
    off_t res = lseek(stream->fd, offset, whence);
    if (res == (off_t)-1) {
        stream->error = 1;
        funlockfile(stream);
        return -1;
    }
    stream->eof = 0;
    funlockfile(stream);
    return 0;
}

long ftell(FILE *stream) {
    if (!stream) {
        errno = EBADF;
        return -1;
    }
    flockfile(stream);
    off_t pos = lseek(stream->fd, 0, SEEK_CUR);
    if (pos == (off_t)-1) {
        funlockfile(stream);
        return -1;
    }
    if (stream->buf_writing && stream->buf) pos += (off_t)stream->buf_pos;
    if (stream->has_ungot) pos--;
    funlockfile(stream);
    return (long)pos;
}

int fseeko(FILE *stream, off_t offset, int whence) {
    return fseek(stream, (long)offset, whence);
}

off_t ftello(FILE *stream) {
    return (off_t)ftell(stream);
}

FILE *popen(const char *command, const char *type) {
    (void)command;
    (void)type;
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *stream) {
    if (!stream) {
        errno = EBADF;
        return -1;
    }
    return fclose(stream);
}

void rewind(FILE *stream) {
    if (stream) {
        fseek(stream, 0, SEEK_SET);
        stream->error = 0;
    }
}

int fgetc(FILE *stream) {
    if (!stream) return EOF;
    flockfile(stream);
    if (stream->buf_writing) flush_stream_locked(stream);
    if (stream->has_ungot) {
        stream->has_ungot = 0;
        int ch = stream->ungotten;
        funlockfile(stream);
        return ch;
    }
    unsigned char ch;
    ssize_t n = read(stream->fd, &ch, 1);
    if (n <= 0) {
        if (n == 0) stream->eof = 1;
        else stream->error = 1;
        funlockfile(stream);
        return EOF;
    }
    funlockfile(stream);
    return (int)ch;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int fputc(int c, FILE *stream) {
    if (!stream) return EOF;
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1u, 1u, stream) != 1u) {
        return EOF;
    }
    return (int)ch;
}

int putc(int c, FILE *stream) {
    return fputc(c, stream);
}

int putchar(int c) {
    return fputc(c, stdout);
}

int getchar(void) {
    return fgetc(stdin);
}

int ungetc(int c, FILE *stream) {
    if (c == EOF || !stream) return EOF;
    flockfile(stream);
    if (stream->has_ungot) {
        funlockfile(stream);
        return EOF;
    }
    stream->ungotten = (unsigned char)c;
    stream->has_ungot = 1;
    stream->eof = 0;
    funlockfile(stream);
    return stream->ungotten;
}

char *fgets(char *s, int size, FILE *stream) {
    if (!s || size <= 0 || !stream) return NULL;
    flockfile(stream);
    int idx = 0;
    while (idx < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (idx == 0) {
                funlockfile(stream);
                return NULL;
            }
            break;
        }
        s[idx++] = (char)c;
        if (c == '\n') break;
    }
    s[idx] = '\0';
    funlockfile(stream);
    return s;
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    size_t len = strlen(s);
    if (fwrite(s, 1, len, stream) != len) return EOF;
    return 0;
}

int puts(const char *s) {
    flockfile(stdout);
    if (fputs(s, stdout) == EOF || putchar('\n') == EOF) {
        funlockfile(stdout);
        return EOF;
    }
    funlockfile(stdout);
    return 0;
}

int feof(FILE *stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream) {
    return stream ? stream->error : 0;
}

void clearerr(FILE *stream) {
    if (stream) {
        flockfile(stream);
        stream->eof = 0;
        stream->error = 0;
        funlockfile(stream);
    }
}

int fflush(FILE *stream) {
    if (stream == NULL) {
        int rc = 0;
        flockfile(stdout);
        if (flush_stream_locked(stdout) != 0) rc = EOF;
        funlockfile(stdout);
        flockfile(stderr);
        if (flush_stream_locked(stderr) != 0) rc = EOF;
        funlockfile(stderr);

        pthread_mutex_lock(&s_file_pool_lock);
        for (size_t i = 0u; i < file_pool_count; i++) {
            FILE *f = file_pool[i];
            if (f && f->fd >= 0) {
                flockfile(f);
                if (flush_stream_locked(f) != 0) rc = EOF;
                funlockfile(f);
            }
        }
        pthread_mutex_unlock(&s_file_pool_lock);
        return rc;
    }
    if (stream->fd < 0) {
        errno = EBADF;
        return EOF;
    }
    flockfile(stream);
    int rc = flush_stream_locked(stream);
    funlockfile(stream);
    return rc;
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size) {
    if (!stream) {
        errno = EBADF;
        return -1;
    }
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
        errno = EINVAL;
        return -1;
    }
    flockfile(stream);
    flush_stream_locked(stream);
    if (stream->buf_owned && stream->buf) {
        free(stream->buf);
    }
    stream->buf = NULL;
    stream->buf_cap = 0u;
    stream->buf_pos = 0u;
    stream->buf_end = 0u;
    stream->buf_owned = 0;
    stream->buf_writing = 0;
    stream->buf_mode = mode;
    if (mode != _IONBF) {
        size_t cap = size ? size : BUFSIZ;
        if (buf) {
            stream->buf = buf;
            stream->buf_cap = cap;
            stream->buf_owned = 0;
        } else {
            stream->buf = (char *)malloc(cap);
            if (!stream->buf) {
                stream->buf_mode = _IONBF;
                funlockfile(stream);
                errno = ENOMEM;
                return -1;
            }
            stream->buf_cap = cap;
            stream->buf_owned = 1;
        }
    }
    funlockfile(stream);
    return 0;
}

void setbuf(FILE *stream, char *buf) {
    (void)setvbuf(stream, buf, buf ? _IOFBF : _IONBF, buf ? BUFSIZ : 0u);
}

static volatile uint32_t s_tmp_counter = 0u;
static char *tmpnam_buf = NULL;
static size_t tmpnam_cap = 0;
static pthread_mutex_t s_tmpnam_lock = PTHREAD_MUTEX_INITIALIZER;

static char *alloc_tmpnam_path(uint32_t id) {
    int needed = snprintf(NULL, 0, "/tmp/tmp_%u.tmp", (unsigned)id);
    if (needed < 0) return NULL;
    char *path = (char *)malloc((size_t)needed + 1);
    if (!path) return NULL;
    snprintf(path, (size_t)needed + 1, "/tmp/tmp_%u.tmp", (unsigned)id);
    return path;
}

char *tmpnam(char *s) {
    uint32_t id = __atomic_add_fetch(&s_tmp_counter, 1u, __ATOMIC_RELAXED);
    if (s) {
        snprintf(s, L_tmpnam, "/tmp/tmp_%u.tmp", (unsigned)id);
        return s;
    }
    pthread_mutex_lock(&s_tmpnam_lock);
    int needed = snprintf(NULL, 0, "/tmp/tmp_%u.tmp", (unsigned)id);
    if (needed < 0) {
        pthread_mutex_unlock(&s_tmpnam_lock);
        return NULL;
    }
    if ((size_t)needed + 1 > tmpnam_cap) {
        char *nb = (char *)realloc(tmpnam_buf, (size_t)needed + 1);
        if (!nb) {
            pthread_mutex_unlock(&s_tmpnam_lock);
            return NULL;
        }
        tmpnam_buf = nb;
        tmpnam_cap = (size_t)needed + 1;
    }
    snprintf(tmpnam_buf, tmpnam_cap, "/tmp/tmp_%u.tmp", (unsigned)id);
    char *res = tmpnam_buf;
    pthread_mutex_unlock(&s_tmpnam_lock);
    return res;
}

FILE *tmpfile(void) {
    mkdir("/tmp", 0755);
    for (int attempt = 0; attempt < 1024; attempt++) {
        uint32_t id = __atomic_add_fetch(&s_tmp_counter, 1u, __ATOMIC_RELAXED);
        char *name = alloc_tmpnam_path(id);
        if (!name) return NULL;
        int fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            int err = errno;
            free(name);
            if (err == EEXIST) continue;
            return NULL;
        }
        FILE *fp = fdopen(fd, "w+");
        if (!fp) {
            close(fd);
            unlink(name);
            free(name);
            return NULL;
        }
        fp->tmp_unlink_path = name;
        return fp;
    }
    errno = EEXIST;
    return NULL;
}

int remove(const char *pathname) {
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }
    if (pathname[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) == 0 && (st.mode & IPO_INODE_TYPE_DIR)) {
        return rmdir(pathname);
    }
    return unlink(pathname);
}

int rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) {
        errno = EINVAL;
        return -1;
    }
    if (oldpath[0] == '\0' || newpath[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    struct ipo_inode old_st;
    if (ipo_stat(oldpath, &old_st) != 0) {
        errno = ENOENT;
        return -1;
    }
    if (old_st.mode & IPO_INODE_FLAG_PROTECTED) {
        errno = EACCES;
        return -1;
    }

    struct ipo_inode new_st;
    if (ipo_stat(newpath, &new_st) == 0) {
        char *rp_old = realpath(oldpath, NULL);
        char *rp_new = realpath(newpath, NULL);
        int same = (rp_old && rp_new && strcmp(rp_old, rp_new) == 0);
        free(rp_old);
        free(rp_new);
        if (same) {
            return 0;
        }

        int old_is_dir = (old_st.mode & IPO_INODE_TYPE_DIR) != 0;
        int new_is_dir = (new_st.mode & IPO_INODE_TYPE_DIR) != 0;
        if (!old_is_dir && new_is_dir) {
            errno = EISDIR;
            return -1;
        }
        if (old_is_dir && !new_is_dir) {
            errno = ENOTDIR;
            return -1;
        }
        if (new_st.mode & IPO_INODE_FLAG_PROTECTED) {
            errno = EACCES;
            return -1;
        }
        if (ipo_delete(newpath) != 0) {
            errno = new_is_dir ? ENOTEMPTY : EACCES;
            return -1;
        }
    }

    uint32_t args[2];
    args[0] = (uint32_t)(uintptr_t)oldpath;
    args[1] = (uint32_t)(uintptr_t)newpath;
    int res = ipo_syscall(IPO_SYSCALL_FS_RENAME, 2u, args);
    if (res != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

void perror(const char *s) {
    if (s && *s) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

/* Printf implementations */

static void uint_to_str(unsigned long long uval, int base, int uppercase, char *buf, int *len) {
    static const char *lower_digits = "0123456789abcdef";
    static const char *upper_digits = "0123456789ABCDEF";
    const char *d = uppercase ? upper_digits : lower_digits;
    int tlen = 0;

    if (uval == 0) {
        buf[tlen++] = '0';
    } else {
        while (uval > 0) {
            buf[tlen++] = d[uval % (unsigned int)base];
            uval /= (unsigned int)base;
        }
        for (int l = 0, r = tlen - 1; l < r; l++, r--) {
            char tmp = buf[l];
            buf[l] = buf[r];
            buf[r] = tmp;
        }
    }

    *len = tlen;
    buf[tlen] = '\0';
}

static void format_uint_str(unsigned long long uval, int base, int uppercase, const char *prefix, char *buf, int *out_len) {
    int plen = prefix ? (int)strlen(prefix) : 0;
    if (plen > 0) memcpy(buf, prefix, (size_t)plen);
    int num_len = 0;
    uint_to_str(uval, base, uppercase, buf + plen, &num_len);
    if (out_len) *out_len = plen + num_len;
}

static char *format_float_alloc(double val, int prec) {
    if (isnan(val)) {
        char *r = (char *)malloc(4u);
        if (r) memcpy(r, "nan", 4u);
        return r;
    }
    if (isinf(val)) {
        const char *s = (val < 0) ? "-inf" : "inf";
        size_t sl = strlen(s) + 1u;
        char *r = (char *)malloc(sl);
        if (r) memcpy(r, s, sl);
        return r;
    }

    if (prec < 0) prec = 6;
    int neg = (val < 0);
    if (neg) val = -val;

    double int_part;
    double frac_part = modf(val, &int_part);
    long long ip = (long long)int_part;
    unsigned long long uip = (ip < 0) ? (0ULL - (unsigned long long)ip) : (unsigned long long)ip;

    int len = 0;
    char int_buf[32];
    uint_to_str(uip, 10, 0, int_buf, &len);

    size_t plen = (size_t)prec;
    if (plen > ((size_t)-1) - (size_t)len - 4u) return NULL;
    size_t total = (size_t)(neg ? 1 : 0) + (size_t)len + (prec > 0 ? (1u + plen) : 0u) + 1u;
    char *buf = (char *)malloc(total);
    if (!buf) return NULL;

    size_t pos = 0;
    if (neg) buf[pos++] = '-';
    memcpy(buf + pos, int_buf, (size_t)len);
    pos += (size_t)len;
    if (prec > 0) {
        buf[pos++] = '.';
        for (int i = 0; i < prec; i++) {
            frac_part *= 10.0;
            int digit = (int)frac_part;
            if (digit < 0) digit = 0;
            if (digit > 9) digit = 9;
            buf[pos++] = (char)('0' + digit);
            frac_part -= digit;
        }
    }
    buf[pos] = '\0';
    return buf;
}

typedef struct {
    char *str;
    size_t size;
    size_t written;
} vsn_stream_t;

static inline void vsn_putc(vsn_stream_t *s, char c) {
    if (s->str && s->written + 1 < s->size) {
        s->str[s->written] = c;
    }
    s->written++;
}

static inline void vsn_puts(vsn_stream_t *s, const char *str, int width, int prec, int left_align, char pad) {
    int len = (int)strlen(str);
    if (prec >= 0 && len > prec) len = prec;
    int pad_len = width > len ? width - len : 0;

    if (!left_align) {
        for (int i = 0; i < pad_len; i++) vsn_putc(s, pad);
    }
    for (int i = 0; i < len; i++) vsn_putc(s, str[i]);
    if (left_align) {
        for (int i = 0; i < pad_len; i++) vsn_putc(s, ' ');
    }
}

static void vsn_put_int(vsn_stream_t *s, const char *digits, int num_len,
                        char sign_ch, int is_zero,
                        int width, int prec, int left_align, int zero_pad) {
    if (prec == 0 && is_zero) {
        num_len = 0;
    }
    int zeros = (prec > num_len) ? (prec - num_len) : 0;
    int sign_len = sign_ch ? 1 : 0;
    int total_len = sign_len + zeros + num_len;
    int pad_len = (width > total_len) ? (width - total_len) : 0;
    int eff_zero_pad = (zero_pad && !left_align && prec < 0);

    if (!left_align && !eff_zero_pad) {
        for (int i = 0; i < pad_len; i++) vsn_putc(s, ' ');
    }
    if (sign_ch) {
        vsn_putc(s, sign_ch);
    }
    if (eff_zero_pad) {
        for (int i = 0; i < pad_len; i++) vsn_putc(s, '0');
    }
    for (int i = 0; i < zeros; i++) vsn_putc(s, '0');
    for (int i = 0; i < num_len; i++) vsn_putc(s, digits[i]);
    if (left_align) {
        for (int i = 0; i < pad_len; i++) vsn_putc(s, ' ');
    }
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    vsn_stream_t stream;
    stream.str = str;
    stream.size = size;
    stream.written = 0;

    const char *p = format;
    while (*p) {
        if (*p != '%') {
            char ch = *p++;
            vsn_putc(&stream, ch);
            continue;
        }
        p++; // skip '%'

        int left_align = 0;
        int plus_sign = 0;
        int space_sign = 0;
        int zero_pad = 0;

        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') {
            if (*p == '-') left_align = 1;
            else if (*p == '+') plus_sign = 1;
            else if (*p == ' ') space_sign = 1;
            else if (*p == '0') zero_pad = 1;
            p++;
        }
        if (left_align) zero_pad = 0;

        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left_align = 1;
                zero_pad = 0;
                width = -width;
            }
            p++;
        } else {
            while (isdigit(*p)) {
                width = width * 10 + (*p - '0');
                p++;
            }
        }

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (isdigit(*p)) {
                    prec = prec * 10 + (*p - '0');
                    p++;
                }
            }
        }

        int is_long = 0;
        int is_long_long = 0;
        if (*p == 'l') {
            p++;
            if (*p == 'l') {
                is_long_long = 1;
                p++;
            } else {
                is_long = 1;
            }
        } else if (*p == 'z') {
            is_long = 1;
            p++;
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') p++;
        }

        char spec = *p++;
        if (!spec) break;
        int num_len = 0;

        switch (spec) {
            case 'd':
            case 'i': {
                long long val = is_long_long ? va_arg(ap, long long) :
                                (is_long ? va_arg(ap, long) : va_arg(ap, int));
                int neg = (val < 0);
                unsigned long long uval = neg ? (0ULL - (unsigned long long)val) : (unsigned long long)val;
                char buf[64];
                format_uint_str(uval, 10, 0, NULL, buf, &num_len);
                char sign_ch = 0;
                if (neg) sign_ch = '-';
                else if (plus_sign) sign_ch = '+';
                else if (space_sign) sign_ch = ' ';
                vsn_put_int(&stream, buf, num_len, sign_ch, uval == 0, width, prec, left_align, zero_pad);
                break;
            }
            case 'u': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) :
                                         (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                char buf[64];
                format_uint_str(val, 10, 0, NULL, buf, &num_len);
                vsn_put_int(&stream, buf, num_len, 0, val == 0, width, prec, left_align, zero_pad);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) :
                                         (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                char buf[64];
                format_uint_str(val, 16, (spec == 'X'), NULL, buf, &num_len);
                vsn_put_int(&stream, buf, num_len, 0, val == 0, width, prec, left_align, zero_pad);
                break;
            }
            case 'o': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) :
                                         (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                char buf[64];
                format_uint_str(val, 8, 0, NULL, buf, &num_len);
                vsn_put_int(&stream, buf, num_len, 0, val == 0, width, prec, left_align, zero_pad);
                break;
            }
            case 'p': {
                void *ptr = va_arg(ap, void *);
                char buf[64];
                format_uint_str((uintptr_t)ptr, 16, 0, "0x", buf, &num_len);
                vsn_puts(&stream, buf, width, prec, left_align, ' ');
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                vsn_puts(&stream, s, width, prec, left_align, ' ');
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                int pad_len = width > 1 ? width - 1 : 0;
                if (!left_align) {
                    for (int i = 0; i < pad_len; i++) vsn_putc(&stream, ' ');
                }
                vsn_putc(&stream, c);
                if (left_align) {
                    for (int i = 0; i < pad_len; i++) vsn_putc(&stream, ' ');
                }
                break;
            }
            case 'f':
            case 'g':
            case 'e': {
                double val = va_arg(ap, double);
                char *buf = format_float_alloc(val, prec);
                if (buf) {
                    vsn_puts(&stream, buf, width, -1, left_align, ' ');
                    free(buf);
                }
                break;
            }
            case '%': {
                vsn_putc(&stream, '%');
                break;
            }
            default:
                vsn_putc(&stream, spec);
                break;
        }
    }

    if (str && size > 0) {
        if (stream.written < size) {
            str[stream.written] = '\0';
        } else {
            str[size - 1] = '\0';
        }
    }
    return (int)stream.written;
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, size, format, ap);
    va_end(ap);
    return res;
}

int sprintf(char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsnprintf(str, (size_t)-1, format, ap);
    va_end(ap);
    return res;
}

int vsprintf(char *str, const char *format, va_list ap) {
    return vsnprintf(str, (size_t)-1, format, ap);
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    if (!stream) return -1;
    va_list aq;
    va_copy(aq, ap);
    int len = vsnprintf(NULL, 0, format, aq);
    va_end(aq);
    if (len < 0) return -1;
    if (len == 0) return 0;
    char *dyn = (char *)malloc((size_t)len + 1u);
    if (!dyn) return -1;
    vsnprintf(dyn, (size_t)len + 1u, format, ap);
    int written = (int)fwrite(dyn, 1, (size_t)len, stream);
    free(dyn);
    return written;
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vfprintf(stream, format, ap);
    va_end(ap);
    return res;
}

int vprintf(const char *format, va_list ap) {
    return vfprintf(stdout, format, ap);
}

int printf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vprintf(format, ap);
    va_end(ap);
    return res;
}

/* Scanf implementations */

int vsscanf(const char *str, const char *format, va_list ap) {
    int matched = 0;
    const char *start = str;
    const char *s = str;
    const char *f = format;

    while (*f) {
        if (isspace(*f)) {
            while (isspace(*f)) f++;
            while (isspace(*s)) s++;
            continue;
        }

        if (*f != '%') {
            if (!*s || *f != *s) break;
            f++;
            s++;
            continue;
        }

        f++; // Skip '%'
        if (*f == '%') {
            if (!*s || *s != '%') break;
            f++;
            s++;
            continue;
        }

        int suppress = 0;
        if (*f == '*') {
            suppress = 1;
            f++;
        }

        int is_long = 0;
        if (*f == 'l') {
            is_long = 1;
            f++;
        }

        char spec = *f++;
        if (spec == 'n') {
            if (!suppress) {
                if (is_long) *va_arg(ap, long *) = (long)(s - start);
                else *va_arg(ap, int *) = (int)(s - start);
            }
            continue;
        }
        if (!*s) break;
        while (isspace(*s) && spec != 'c') s++;
        if (!*s) break;

        switch (spec) {
            case 'd':
            case 'i': {
                char *end;
                long val = strtol(s, &end, (spec == 'i' ? 0 : 10));
                if (end == s) return matched;
                if (!suppress) {
                    if (is_long) *va_arg(ap, long *) = val;
                    else *va_arg(ap, int *) = (int)val;
                    matched++;
                }
                s = end;
                break;
            }
            case 'u': {
                char *end;
                unsigned long val = strtoul(s, &end, 10);
                if (end == s) return matched;
                if (!suppress) {
                    if (is_long) *va_arg(ap, unsigned long *) = val;
                    else *va_arg(ap, unsigned int *) = (unsigned int)val;
                    matched++;
                }
                s = end;
                break;
            }
            case 'x':
            case 'X': {
                char *end;
                unsigned long val = strtoul(s, &end, 16);
                if (end == s) return matched;
                if (!suppress) {
                    if (is_long) *va_arg(ap, unsigned long *) = val;
                    else *va_arg(ap, unsigned int *) = (unsigned int)val;
                    matched++;
                }
                s = end;
                break;
            }
            case 's': {
                char *out = suppress ? NULL : va_arg(ap, char *);
                int idx = 0;
                while (*s && !isspace(*s)) {
                    if (out) out[idx++] = *s++;
                    else s++;
                }
                if (out) {
                    out[idx] = '\0';
                    if (idx > 0) matched++;
                }
                break;
            }
            case 'c': {
                if (suppress) {
                    s++;
                } else {
                    char *out = va_arg(ap, char *);
                    *out = *s++;
                    matched++;
                }
                break;
            }
            case 'f': {
                char *end;
                double val = strtod(s, &end);
                if (end == s) return matched;
                if (!suppress) {
                    if (is_long) *va_arg(ap, double *) = val;
                    else *va_arg(ap, float *) = (float)val;
                    matched++;
                }
                s = end;
                break;
            }
            default:
                return matched;
        }
    }
    return matched;
}

int sscanf(const char *str, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = vsscanf(str, format, ap);
    va_end(ap);
    return res;
}

static char *read_dynamic_line(FILE *stream) {
    if (!stream) return NULL;
    size_t cap = 128;
    size_t len = 0;
    char *line = (char *)malloc(cap);
    if (!line) return NULL;

    int ch;
    while ((ch = fgetc(stream)) != EOF) {
        if (len + 2 > cap) {
            size_t new_cap = cap * 2;
            char *nb = (char *)realloc(line, new_cap);
            if (!nb) {
                free(line);
                return NULL;
            }
            line = nb;
            cap = new_cap;
        }
        line[len++] = (char)ch;
        if (ch == '\n') break;
    }

    if (len == 0) {
        free(line);
        return NULL;
    }
    line[len] = '\0';
    return line;
}

int fscanf(FILE *stream, const char *format, ...) {
    char *line = read_dynamic_line(stream);
    if (!line) return EOF;
    va_list ap;
    va_start(ap, format);
    int res = vsscanf(line, format, ap);
    va_end(ap);
    free(line);
    return res;
}

int scanf(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int res = fscanf(stdin, format, ap);
    va_end(ap);
    return res;
}

int serial_printf(const char *fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    int ret = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (ret < 0) {
        va_end(ap);
        return -1;
    }
    if (ret > 0) {
        char *buf = (char *)malloc((size_t)ret + 1u);
        if (buf) {
            vsnprintf(buf, (size_t)ret + 1u, fmt, ap);
            uint32_t args[3] = { (uint32_t)(uintptr_t)buf, (uint32_t)ret, 0x53455249u };
            ipo_syscall(IPO_SYSCALL_WRITE, 3u, args);
            free(buf);
        } else {
            char fallback[64];
            vsnprintf(fallback, sizeof(fallback), fmt, ap);
            int flen = (ret < (int)sizeof(fallback)) ? ret : (int)sizeof(fallback) - 1;
            if (flen > 0) {
                uint32_t args[3] = { (uint32_t)(uintptr_t)fallback, (uint32_t)flen, 0x53455249u };
                ipo_syscall(IPO_SYSCALL_WRITE, 3u, args);
            }
        }
    }
    va_end(ap);
    return ret;
}
