#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <math.h>
#include <syscall.h>

static FILE **file_pool = NULL;
static size_t file_pool_count = 0;
static size_t file_pool_cap = 0;

static FILE std_streams[3] = {
    { .fd = 0, .eof = 0, .error = 0, .ungotten = 0, .has_ungot = 0 },
    { .fd = 1, .eof = 0, .error = 0, .ungotten = 0, .has_ungot = 0 },
    { .fd = 2, .eof = 0, .error = 0, .ungotten = 0, .has_ungot = 0 }
};

FILE *stdin = &std_streams[0];
FILE *stdout = &std_streams[1];
FILE *stderr = &std_streams[2];

FILE *fopen(const char *pathname, const char *mode) {
    if (!pathname || !mode) {
        errno = EINVAL;
        return NULL;
    }

    int flags = 0;
    if (strchr(mode, 'w')) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (strchr(mode, '+')) flags = O_RDWR | O_CREAT | O_TRUNC;
    } else if (strchr(mode, 'a')) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        if (strchr(mode, '+')) flags = O_RDWR | O_CREAT | O_APPEND;
    } else { // 'r'
        flags = O_RDONLY;
        if (strchr(mode, '+')) flags = O_RDWR;
    }

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
    f->eof = 0;
    f->error = 0;
    f->ungotten = 0;
    f->has_ungot = 0;

    if (file_pool_count >= file_pool_cap) {
        size_t new_cap = (file_pool_cap == 0) ? 16 : file_pool_cap * 2;
        FILE **new_pool = (FILE **)realloc(file_pool, new_cap * sizeof(FILE *));
        if (!new_pool) {
            free(f);
            errno = ENOMEM;
            return NULL;
        }
        file_pool = new_pool;
        file_pool_cap = new_cap;
    }

    file_pool[file_pool_count++] = f;
    return f;
}

FILE *freopen(const char *pathname, const char *mode, FILE *stream) {
    if (stream) {
        fclose(stream);
    }
    return fopen(pathname, mode);
}

int fclose(FILE *stream) {
    if (!stream) {
        errno = EBADF;
        return EOF;
    }

    int res = close(stream->fd);
    for (size_t i = 0; i < file_pool_count; i++) {
        if (file_pool[i] == stream) {
            file_pool[i] = file_pool[file_pool_count - 1];
            file_pool_count--;
            free(stream);
            break;
        }
    }
    return (res < 0) ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    size_t total = size * nmemb;
    size_t bytes_read = 0;
    char *out = (char *)ptr;

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
        bytes_read += n;
        out += n;
    }

    return bytes_read / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    size_t total = size * nmemb;
    size_t bytes_written = 0;
    const char *in = (const char *)ptr;

    while (bytes_written < total) {
        ssize_t n = write(stream->fd, in, total - bytes_written);
        if (n <= 0) {
            stream->error = 1;
            break;
        }
        bytes_written += n;
        in += n;
    }

    return bytes_written / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) {
        errno = EBADF;
        return -1;
    }
    if (whence == SEEK_CUR && stream->has_ungot) {
        offset--;
    }
    stream->has_ungot = 0;
    off_t res = lseek(stream->fd, offset, whence);
    if (res == (off_t)-1) {
        stream->error = 1;
        return -1;
    }
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream) {
    if (!stream) {
        errno = EBADF;
        return -1;
    }
    off_t pos = lseek(stream->fd, 0, SEEK_CUR);
    if (pos == (off_t)-1) return -1;
    if (stream->has_ungot) pos--;
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
    if (stream->has_ungot) {
        stream->has_ungot = 0;
        return stream->ungotten;
    }
    unsigned char ch;
    ssize_t n = read(stream->fd, &ch, 1);
    if (n <= 0) {
        if (n == 0) stream->eof = 1;
        else stream->error = 1;
        return EOF;
    }
    return (int)ch;
}

int getc(FILE *stream) {
    return fgetc(stream);
}

int fputc(int c, FILE *stream) {
    if (!stream) return EOF;
    unsigned char ch = (unsigned char)c;
    ssize_t n = write(stream->fd, &ch, 1);
    if (n != 1) {
        stream->error = 1;
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
    if (c == EOF || !stream || stream->has_ungot) return EOF;
    stream->ungotten = (unsigned char)c;
    stream->has_ungot = 1;
    stream->eof = 0;
    return stream->ungotten;
}

char *fgets(char *s, int size, FILE *stream) {
    if (!s || size <= 0 || !stream) return NULL;
    int idx = 0;
    while (idx < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (idx == 0) return NULL;
            break;
        }
        s[idx++] = (char)c;
        if (c == '\n') break;
    }
    s[idx] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream) {
    if (!s || !stream) return EOF;
    size_t len = strlen(s);
    if (fwrite(s, 1, len, stream) != len) return EOF;
    return 0;
}

int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    if (putchar('\n') == EOF) return EOF;
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
        stream->eof = 0;
        stream->error = 0;
    }
}

int fflush(FILE *stream) {
    if (stream != NULL) {
        if (stream->fd < 0) {
            errno = EBADF;
            return EOF;
        }
    }
    return 0;
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
    (void)buf;
    (void)size;
    return 0;
}

void setbuf(FILE *stream, char *buf) {
    (void)setvbuf(stream, buf, buf ? _IOFBF : _IONBF, buf ? BUFSIZ : 0u);
}

static int tmp_counter = 0;
static char *tmpnam_buf = NULL;
static size_t tmpnam_cap = 0;

static char *alloc_tmpnam_path(int id) {
    int needed = snprintf(NULL, 0, "/tmp/tmp_%d.tmp", id);
    if (needed < 0) return NULL;
    char *path = (char *)malloc((size_t)needed + 1);
    if (!path) return NULL;
    snprintf(path, (size_t)needed + 1, "/tmp/tmp_%d.tmp", id);
    return path;
}

char *tmpnam(char *s) {
    int id = ++tmp_counter;
    if (s) {
        snprintf(s, L_tmpnam, "/tmp/tmp_%d.tmp", id);
        return s;
    }
    int needed = snprintf(NULL, 0, "/tmp/tmp_%d.tmp", id);
    if (needed < 0) return NULL;
    if ((size_t)needed + 1 > tmpnam_cap) {
        char *nb = (char *)realloc(tmpnam_buf, (size_t)needed + 1);
        if (!nb) return NULL;
        tmpnam_buf = nb;
        tmpnam_cap = (size_t)needed + 1;
    }
    snprintf(tmpnam_buf, tmpnam_cap, "/tmp/tmp_%d.tmp", id);
    return tmpnam_buf;
}

FILE *tmpfile(void) {
    char *name = alloc_tmpnam_path(++tmp_counter);
    if (!name) return NULL;
    FILE *fp = fopen(name, "w+");
    free(name);
    return fp;
}

int remove(const char *pathname) {
    return unlink(pathname);
}

int rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) {
        errno = EINVAL;
        return -1;
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

static void format_float_str(double val, int prec, char *buf, size_t buf_size) {
    if (isnan(val)) {
        if (buf_size > 3) strcpy(buf, "nan");
        return;
    }
    if (isinf(val)) {
        if (val < 0 && buf_size > 4) strcpy(buf, "-inf");
        else if (buf_size > 3) strcpy(buf, "inf");
        return;
    }

    if (prec < 0) prec = 6;
    if (prec > 16) prec = 16;
    int neg = (val < 0);
    if (neg) val = -val;

    double int_part;
    double frac_part = modf(val, &int_part);
    long long ip = (long long)int_part;
    unsigned long long uip = (ip < 0) ? (0ULL - (unsigned long long)ip) : (unsigned long long)ip;

    char *out = buf;
    char *end = buf + buf_size - 1;
    if (neg && out < end) *out++ = '-';
    int len = 0;
    char int_buf[32];
    uint_to_str(uip, 10, 0, int_buf, &len);
    for (int i = 0; i < len && out < end; i++) *out++ = int_buf[i];

    if (prec > 0 && out < end) {
        *out++ = '.';
        for (int i = 0; i < prec && out < end; i++) {
            frac_part *= 10.0;
            int digit = (int)frac_part;
            *out++ = '0' + (digit % 10);
            frac_part -= digit;
        }
    }
    *out = '\0';
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
        char pad_char = ' ';

        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') {
            if (*p == '-') left_align = 1;
            else if (*p == '+') plus_sign = 1;
            else if (*p == ' ') space_sign = 1;
            else if (*p == '0') pad_char = '0';
            p++;
        }
        if (left_align) pad_char = ' ';

        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
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
        }

        char spec = *p++;
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

                if (sign_ch) {
                    if (width > 0) width--;
                    if (pad_char == '0') {
                        vsn_putc(&stream, sign_ch);
                        vsn_puts(&stream, buf, width, prec, left_align, '0');
                    } else {
                        if (!left_align && width > num_len) {
                            for (int i = 0; i < width - num_len; i++) vsn_putc(&stream, ' ');
                            vsn_putc(&stream, sign_ch);
                            for (int i = 0; i < num_len; i++) vsn_putc(&stream, buf[i]);
                        } else {
                            vsn_putc(&stream, sign_ch);
                            vsn_puts(&stream, buf, width, prec, left_align, ' ');
                        }
                    }
                } else {
                    vsn_puts(&stream, buf, width, prec, left_align, pad_char);
                }
                break;
            }
            case 'u': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) :
                                         (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                char buf[64];
                format_uint_str(val, 10, 0, NULL, buf, &num_len);
                vsn_puts(&stream, buf, width, prec, left_align, pad_char);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) :
                                         (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                char buf[64];
                format_uint_str(val, 16, (spec == 'X'), NULL, buf, &num_len);
                vsn_puts(&stream, buf, width, prec, left_align, pad_char);
                break;
            }
            case 'o': {
                unsigned long long val = is_long_long ? va_arg(ap, unsigned long long) :
                                         (is_long ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int));
                char buf[64];
                format_uint_str(val, 8, 0, NULL, buf, &num_len);
                vsn_puts(&stream, buf, width, prec, left_align, pad_char);
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
                char buf[64];
                format_float_str(val, prec, buf, sizeof(buf));
                vsn_puts(&stream, buf, width, -1, left_align, ' ');
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
    char stack_buf[1024];
    va_list aq;
    va_copy(aq, ap);
    int len = vsnprintf(stack_buf, sizeof(stack_buf), format, aq);
    va_end(aq);
    if (len < 0) return -1;
    if ((size_t)len < sizeof(stack_buf)) {
        return (int)fwrite(stack_buf, 1, (size_t)len, stream);
    }
    char *dyn = (char *)malloc((size_t)len + 1);
    if (!dyn) return -1;
    vsnprintf(dyn, (size_t)len + 1, format, ap);
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
    const char *s = str;
    const char *f = format;

    while (*f && *s) {
        if (isspace(*f)) {
            while (isspace(*f)) f++;
            while (isspace(*s)) s++;
            continue;
        }

        if (*f != '%') {
            if (*f != *s) break;
            f++;
            s++;
            continue;
        }

        f++; // Skip '%'
        if (*f == '%') {
            if (*s != '%') break;
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
        while (isspace(*s) && spec != 'c') s++;

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
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (ret < 0) return -1;
    int limit = (ret < (int)sizeof(buf)) ? ret : (int)sizeof(buf) - 1;
    for (int i = 0; i < limit && buf[i]; i++) {
#ifdef __i386__
        __asm__ volatile("outb %0, %1" : : "a"((uint8_t)buf[i]), "Nd"((uint16_t)0x3F8));
#endif
    }
    return ret;
}
