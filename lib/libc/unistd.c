#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <syscall.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/select.h>

#ifndef IPO_INODE_FLAG_PROTECTED
#define IPO_INODE_FLAG_PROTECTED 0x80000000u
#endif

#define IPO_O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)

extern int ipo_libc_socket_is_fd(int fd) __attribute__((weak));
extern ssize_t ipo_libc_socket_read(int fd, void *buf, size_t count) __attribute__((weak));
extern ssize_t ipo_libc_socket_write(int fd, const void *buf, size_t count) __attribute__((weak));
extern int ipo_libc_socket_close(int fd) __attribute__((weak));
extern ssize_t ipo_libc_stdin_read(void *buf, size_t count) __attribute__((weak));

static int is_socket_fd(int fd) {
    return ipo_libc_socket_is_fd && ipo_libc_socket_is_fd(fd);
}

typedef enum {
    IPO_FD_NONE = 0,
    IPO_FD_CONSOLE_IN,
    IPO_FD_CONSOLE_OUT,
    IPO_FD_CONSOLE_ERR,
    IPO_FD_FILE,
    IPO_FD_PIPE,
    IPO_FD_DEVNULL
} ipo_fd_kind_t;

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;
    size_t tail;
    size_t len;
    size_t readers;
    size_t writers;
    pthread_mutex_t lock;
    pthread_cond_t can_read;
    pthread_cond_t can_write;
} ipo_pipe_t;

typedef struct {
    ipo_fd_kind_t kind;
    uint32_t ref_count;
    int open_flags;
    int kfd;
    int pipe_is_writer;
    int heap_allocated;
    char *path;
    ipo_pipe_t *pipe;
    pthread_mutex_t io_lock;
} ipo_ofd_t;

typedef struct {
    ipo_ofd_t *ofd;
    int fd_flags;
} ipo_fd_slot_t;

static pthread_mutex_t s_fd_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_mutex_t s_stdin_lock = PTHREAD_MUTEX_INITIALIZER;
static ipo_fd_slot_t *s_fd_slots = NULL;
static size_t s_fd_slots_cap = 0;
static int s_fd_initialized = 0;

static ipo_ofd_t s_stdio_ofds[3] = {
    { IPO_FD_CONSOLE_IN,  1u, O_RDONLY, 0, 0, 0, NULL, NULL, PTHREAD_MUTEX_INITIALIZER },
    { IPO_FD_CONSOLE_OUT, 1u, O_WRONLY, 1, 0, 0, NULL, NULL, PTHREAD_MUTEX_INITIALIZER },
    { IPO_FD_CONSOLE_ERR, 1u, O_WRONLY, 2, 0, 0, NULL, NULL, PTHREAD_MUTEX_INITIALIZER }
};

static int ensure_fd_cap_locked(size_t need_cap) {
    if (need_cap <= s_fd_slots_cap) return 0;
    size_t new_cap = s_fd_slots_cap ? s_fd_slots_cap : 64u;
    while (new_cap < need_cap) {
        if (new_cap > ((size_t)-1) / (2u * sizeof(ipo_fd_slot_t))) return -1;
        new_cap *= 2u;
    }
    ipo_fd_slot_t *nb = (ipo_fd_slot_t *)realloc(s_fd_slots, new_cap * sizeof(ipo_fd_slot_t));
    if (!nb) return -1;
    memset(nb + s_fd_slots_cap, 0, (new_cap - s_fd_slots_cap) * sizeof(ipo_fd_slot_t));
    s_fd_slots = nb;
    s_fd_slots_cap = new_cap;
    return 0;
}

static void ensure_fd_init_locked(void) {
    if (s_fd_initialized) return;
    if (ensure_fd_cap_locked(64u) != 0) return;
    s_stdio_ofds[0].ref_count = 1u;
    s_stdio_ofds[1].ref_count = 1u;
    s_stdio_ofds[2].ref_count = 1u;
    s_fd_slots[0].ofd = &s_stdio_ofds[0];
    s_fd_slots[1].ofd = &s_stdio_ofds[1];
    s_fd_slots[2].ofd = &s_stdio_ofds[2];
    s_fd_initialized = 1;
}

static void ofd_retain(ipo_ofd_t *ofd) {
    if (!ofd) return;
    __atomic_add_fetch(&ofd->ref_count, 1u, __ATOMIC_RELAXED);
}

static void ofd_release(ipo_ofd_t *ofd) {
    if (!ofd) return;
    if (__atomic_sub_fetch(&ofd->ref_count, 1u, __ATOMIC_ACQ_REL) != 0u) {
        return;
    }
    if (ofd->kind == IPO_FD_FILE && ofd->kfd >= 0) {
        ipo_close(ofd->kfd);
        ofd->kfd = -1;
    } else if (ofd->kind == IPO_FD_PIPE && ofd->pipe) {
        ipo_pipe_t *p = ofd->pipe;
        int free_pipe = 0;
        pthread_mutex_lock(&p->lock);
        if (ofd->pipe_is_writer) {
            if (p->writers > 0u) p->writers--;
            pthread_cond_broadcast(&p->can_read);
        } else {
            if (p->readers > 0u) p->readers--;
            pthread_cond_broadcast(&p->can_write);
        }
        if (p->readers == 0u && p->writers == 0u) {
            free_pipe = 1;
        }
        pthread_mutex_unlock(&p->lock);
        if (free_pipe) {
            pthread_cond_destroy(&p->can_read);
            pthread_cond_destroy(&p->can_write);
            pthread_mutex_destroy(&p->lock);
            free(p->buf);
            free(p);
        }
        ofd->pipe = NULL;
    }
    if (ofd->path) {
        free(ofd->path);
        ofd->path = NULL;
    }
    if (ofd->heap_allocated) {
        pthread_mutex_destroy(&ofd->io_lock);
        free(ofd);
    }
}

static ipo_ofd_t *ofd_acquire_for_fd(int fd) {
    if (fd < 0) return NULL;
    pthread_mutex_lock(&s_fd_lock);
    ensure_fd_init_locked();
    ipo_ofd_t *ofd = ((size_t)fd < s_fd_slots_cap && s_fd_slots) ? s_fd_slots[fd].ofd : NULL;
    if (ofd) {
        ofd_retain(ofd);
    }
    pthread_mutex_unlock(&s_fd_lock);
    return ofd;
}

static int alloc_fd_slot_locked(int min_fd, int preferred_fd) {
    ensure_fd_init_locked();
    if (min_fd < 0) min_fd = 0;
    if (preferred_fd >= min_fd) {
        if (ensure_fd_cap_locked((size_t)preferred_fd + 1u) == 0 &&
            s_fd_slots[preferred_fd].ofd == NULL && !is_socket_fd(preferred_fd)) {
            return preferred_fd;
        }
    }
    for (size_t i = (size_t)min_fd; i < s_fd_slots_cap; i++) {
        if (s_fd_slots[i].ofd == NULL && !is_socket_fd((int)i)) {
            return (int)i;
        }
    }
    size_t candidate = s_fd_slots_cap;
    while (1) {
        if (ensure_fd_cap_locked(candidate + 1u) != 0) return -1;
        if (s_fd_slots[candidate].ofd == NULL && !is_socket_fd((int)candidate)) {
            return (int)candidate;
        }
        candidate++;
    }
}

static ipo_ofd_t *ofd_alloc(ipo_fd_kind_t kind, int open_flags) {
    ipo_ofd_t *ofd = (ipo_ofd_t *)calloc(1u, sizeof(ipo_ofd_t));
    if (!ofd) return NULL;
    ofd->kind = kind;
    ofd->ref_count = 1u;
    ofd->open_flags = open_flags;
    ofd->kfd = -1;
    ofd->heap_allocated = 1;
    pthread_mutex_init(&ofd->io_lock, NULL);
    return ofd;
}

void ipo_libc_set_fd_path(int fd, const char *path) {
    if (fd < 0) return;
    char *resolved = NULL;
    if (path) {
        resolved = realpath(path, NULL);
        if (!resolved) resolved = strdup(path);
    }
    pthread_mutex_lock(&s_fd_lock);
    ensure_fd_init_locked();
    if ((size_t)fd < s_fd_slots_cap && s_fd_slots && s_fd_slots[fd].ofd) {
        ipo_ofd_t *ofd = s_fd_slots[fd].ofd;
        free(ofd->path);
        ofd->path = resolved;
        resolved = NULL;
    }
    pthread_mutex_unlock(&s_fd_lock);
    free(resolved);
}

const char *ipo_libc_get_fd_path(int fd) {
    if (fd < 0) return NULL;
    pthread_mutex_lock(&s_fd_lock);
    ensure_fd_init_locked();
    const char *res = NULL;
    if ((size_t)fd < s_fd_slots_cap && s_fd_slots && s_fd_slots[fd].ofd) {
        res = s_fd_slots[fd].ofd->path;
    }
    pthread_mutex_unlock(&s_fd_lock);
    return res;
}

int ipo_libc_fd_is_open(int fd) {
    if (fd < 0) return 0;
    pthread_mutex_lock(&s_fd_lock);
    ensure_fd_init_locked();
    int open_ok = ((size_t)fd < s_fd_slots_cap && s_fd_slots && s_fd_slots[fd].ofd != NULL);
    pthread_mutex_unlock(&s_fd_lock);
    return open_ok || is_socket_fd(fd);
}

int ipo_libc_fd_get_kfd(int fd, int require_write) {
    ipo_ofd_t *ofd = ofd_acquire_for_fd(fd);
    if (!ofd) {
        errno = EBADF;
        return -1;
    }
    if (ofd->kind != IPO_FD_FILE || ofd->kfd < 0) {
        ofd_release(ofd);
        errno = EINVAL;
        return -1;
    }
    if (require_write && (ofd->open_flags & IPO_O_ACCMODE) == O_RDONLY) {
        ofd_release(ofd);
        errno = EINVAL;
        return -1;
    }
    int kfd = ofd->kfd;
    ofd_release(ofd);
    return kfd;
}

int ipo_libc_fd_fstat(int fd, struct stat *statbuf) {
    if (fd < 0 || !statbuf) {
        errno = !statbuf ? EFAULT : EBADF;
        return -1;
    }
    ipo_ofd_t *ofd = ofd_acquire_for_fd(fd);
    if (!ofd) {
        if (is_socket_fd(fd)) {
            memset(statbuf, 0, sizeof(struct stat));
            statbuf->st_mode = S_IFSOCK | 0666;
            statbuf->st_nlink = 1;
            return 0;
        }
        errno = EBADF;
        return -1;
    }

    memset(statbuf, 0, sizeof(struct stat));
    statbuf->st_nlink = 1;

    if (ofd->kind == IPO_FD_CONSOLE_IN || ofd->kind == IPO_FD_CONSOLE_OUT ||
        ofd->kind == IPO_FD_CONSOLE_ERR || ofd->kind == IPO_FD_DEVNULL) {
        statbuf->st_mode = S_IFCHR | 0666;
    } else if (ofd->kind == IPO_FD_PIPE && ofd->pipe) {
        pthread_mutex_lock(&ofd->pipe->lock);
        size_t len = ofd->pipe->len;
        pthread_mutex_unlock(&ofd->pipe->lock);
        statbuf->st_mode = S_IFIFO | 0600;
        statbuf->st_size = (int32_t)len;
    } else if (ofd->kind == IPO_FD_FILE && ofd->kfd >= 0) {
        pthread_mutex_lock(&ofd->io_lock);
        int64_t cur = ipo_lseek64(ofd->kfd, 0, SEEK_CUR);
        int64_t size = ipo_lseek64(ofd->kfd, 0, SEEK_END);
        if (cur >= 0) {
            ipo_lseek64(ofd->kfd, cur, SEEK_SET);
        }
        pthread_mutex_unlock(&ofd->io_lock);
        statbuf->st_mode = S_IFREG | 0644;
        statbuf->st_size = (size < 0) ? 0 : (int32_t)size;
    } else {
        ofd_release(ofd);
        errno = EBADF;
        return -1;
    }

    ofd_release(ofd);
    return 0;
}

int ipo_libc_parent_dir_errno(const char *pathname) {
    if (!pathname || pathname[0] == '\0') return ENOENT;
    char *copy = strdup(pathname);
    if (!copy) return ENOENT;
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') {
        copy[--len] = '\0';
    }
    char *slash = strrchr(copy, '/');
    while (slash && slash != copy) {
        *slash = '\0';
        struct ipo_inode pst;
        if (ipo_stat(copy, &pst) == 0) {
            int is_dir = (pst.mode & IPO_INODE_TYPE_DIR) != 0;
            free(copy);
            return is_dir ? ENOENT : ENOTDIR;
        }
        slash = strrchr(copy, '/');
    }
    free(copy);
    return ENOENT;
}

int open(const char *pathname, int flags, ...) {
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }
    if (pathname[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    if (strcmp(pathname, "/dev/null") == 0) {
        ipo_ofd_t *ofd = ofd_alloc(IPO_FD_DEVNULL, flags);
        if (!ofd) {
            errno = ENOMEM;
            return -1;
        }
        pthread_mutex_lock(&s_fd_lock);
        int ufd = alloc_fd_slot_locked(3, -1);
        if (ufd < 0) {
            pthread_mutex_unlock(&s_fd_lock);
            ofd_release(ofd);
            errno = EMFILE;
            return -1;
        }
        s_fd_slots[ufd].ofd = ofd;
        s_fd_slots[ufd].fd_flags = 0;
        pthread_mutex_unlock(&s_fd_lock);
        return ufd;
    }

    size_t plen = strlen(pathname);
    int has_trailing_slash = (plen > 1 && pathname[plen - 1] == '/') || strcmp(pathname, "/") == 0;

    struct ipo_inode st;
    int exists = (ipo_stat(pathname, &st) == 0);

    if (exists) {
        if (st.mode & IPO_INODE_TYPE_DIR) {
            errno = EISDIR;
            return -1;
        }
        if (has_trailing_slash) {
            errno = ENOTDIR;
            return -1;
        }
        if ((flags & (O_WRONLY | O_RDWR | O_TRUNC)) && (st.mode & IPO_INODE_FLAG_PROTECTED)) {
            errno = EACCES;
            return -1;
        }
    } else {
        int perr = ipo_libc_parent_dir_errno(pathname);
        if (perr == ENOTDIR) {
            errno = ENOTDIR;
            return -1;
        }
        if (has_trailing_slash) {
            errno = (flags & O_CREAT) ? EISDIR : ENOENT;
            return -1;
        }
        if (!(flags & O_CREAT)) {
            errno = ENOENT;
            return -1;
        }
    }

    if (flags & O_CREAT) {
        if (!exists) {
            if (ipo_create(pathname, 0x2 /* IPO_INODE_TYPE_FILE */) < 0) {
                errno = ipo_libc_parent_dir_errno(pathname);
                return -1;
            }
        } else if (flags & O_EXCL) {
            errno = EEXIST;
            return -1;
        }
    }

    int kfd = ipo_open(pathname);
    if (kfd < 0) {
        errno = ENOENT;
        return -1;
    }

    if ((flags & O_TRUNC) && ((flags & IPO_O_ACCMODE) != O_RDONLY)) {
        if (ipo_ftruncate64(kfd, 0) < 0) {
            ipo_close(kfd);
            errno = EIO;
            return -1;
        }
    }

    if (flags & O_APPEND) {
        ipo_lseek64(kfd, 0, SEEK_END);
    } else {
        ipo_lseek64(kfd, 0, SEEK_SET);
    }

    ipo_ofd_t *ofd = ofd_alloc(IPO_FD_FILE, flags);
    if (!ofd) {
        ipo_close(kfd);
        errno = ENOMEM;
        return -1;
    }
    ofd->kfd = kfd;
    char *rp = realpath(pathname, NULL);
    ofd->path = rp ? rp : strdup(pathname);

    pthread_mutex_lock(&s_fd_lock);
    int ufd = alloc_fd_slot_locked(3, kfd);
    if (ufd < 0) {
        pthread_mutex_unlock(&s_fd_lock);
        ofd_release(ofd);
        errno = EMFILE;
        return -1;
    }
    s_fd_slots[ufd].ofd = ofd;
    s_fd_slots[ufd].fd_flags = 0;
    pthread_mutex_unlock(&s_fd_lock);

    return ufd;
}

int close(int fd) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    pthread_mutex_lock(&s_fd_lock);
    ensure_fd_init_locked();
    ipo_ofd_t *ofd = NULL;
    if ((size_t)fd < s_fd_slots_cap && s_fd_slots && s_fd_slots[fd].ofd) {
        ofd = s_fd_slots[fd].ofd;
        s_fd_slots[fd].ofd = NULL;
        s_fd_slots[fd].fd_flags = 0;
    }
    pthread_mutex_unlock(&s_fd_lock);

    if (ofd) {
        ofd_release(ofd);
        return 0;
    }

    if (is_socket_fd(fd)) {
        return ipo_libc_socket_close ? ipo_libc_socket_close(fd) : -1;
    }

    errno = EBADF;
    return -1;
}

static char *stdin_line_buf = NULL;
static size_t stdin_line_cap = 0;
static size_t stdin_line_len = 0;
static size_t stdin_line_pos = 0;

static ssize_t console_stdin_read(void *buf, size_t count) {
    if (ipo_libc_stdin_read) {
        return ipo_libc_stdin_read(buf, count);
    }
    pthread_mutex_lock(&s_stdin_lock);
    if (stdin_line_pos >= stdin_line_len) {
        stdin_line_pos = 0;
        stdin_line_len = 0;
        char *line = NULL;
        int n = ipo_read_line_dynamic(&line);
        if (n == -2) {
            pthread_mutex_unlock(&s_stdin_lock);
            errno = EINTR;
            return -1;
        }
        if (n == -3) {
            pthread_mutex_unlock(&s_stdin_lock);
            return 0;
        }
        if (n < 0 || line == NULL) {
            pthread_mutex_unlock(&s_stdin_lock);
            errno = EIO;
            return -1;
        }
        size_t needed = (size_t)n + 2u;
        if (stdin_line_cap < needed) {
            char *new_buf = (char *)realloc(stdin_line_buf, needed);
            if (!new_buf) {
                ipo_kfree(line);
                pthread_mutex_unlock(&s_stdin_lock);
                errno = ENOMEM;
                return -1;
            }
            stdin_line_buf = new_buf;
            stdin_line_cap = needed;
        }
        memcpy(stdin_line_buf, line, (size_t)n);
        stdin_line_buf[n] = '\n';
        stdin_line_buf[n + 1] = '\0';
        stdin_line_len = (size_t)n + 1u;
        ipo_kfree(line);
    }

    size_t avail = stdin_line_len - stdin_line_pos;
    size_t to_read = count < avail ? count : avail;
    memcpy(buf, stdin_line_buf + stdin_line_pos, to_read);
    stdin_line_pos += to_read;
    pthread_mutex_unlock(&s_stdin_lock);
    return (ssize_t)to_read;
}

static ssize_t pipe_read_ofd(ipo_ofd_t *ofd, void *buf, size_t count) {
    ipo_pipe_t *p = ofd->pipe;
    if (!p) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&p->lock);
    while (p->len == 0u) {
        if (p->writers == 0u) {
            pthread_mutex_unlock(&p->lock);
            return 0;
        }
        if (ofd->open_flags & O_NONBLOCK) {
            pthread_mutex_unlock(&p->lock);
            errno = EAGAIN;
            return -1;
        }
        pthread_cond_wait(&p->can_read, &p->lock);
    }
    size_t to_read = count < p->len ? count : p->len;
    size_t first = p->cap - p->head;
    if (first > to_read) first = to_read;
    memcpy(buf, p->buf + p->head, first);
    if (to_read > first) {
        memcpy((uint8_t *)buf + first, p->buf, to_read - first);
    }
    p->head = (p->head + to_read) % p->cap;
    p->len -= to_read;
    pthread_cond_broadcast(&p->can_write);
    pthread_mutex_unlock(&p->lock);
    return (ssize_t)to_read;
}

static int pipe_ensure_space_locked(ipo_pipe_t *p, size_t need) {
    if (p->cap - p->len >= need) return 0;
    size_t min_cap = p->len + need;
    if (min_cap < p->len) return -1;
    size_t new_cap = p->cap ? p->cap : 4096u;
    while (new_cap < min_cap) {
        if (new_cap > ((size_t)-1) / 2u) {
            new_cap = min_cap;
            break;
        }
        new_cap *= 2u;
    }
    uint8_t *nb = (uint8_t *)malloc(new_cap);
    if (!nb) return -1;
    if (p->len > 0u) {
        size_t first = p->cap - p->head;
        if (first > p->len) first = p->len;
        memcpy(nb, p->buf + p->head, first);
        if (p->len > first) {
            memcpy(nb + first, p->buf, p->len - first);
        }
    }
    free(p->buf);
    p->buf = nb;
    p->cap = new_cap;
    p->head = 0u;
    p->tail = p->len;
    return 0;
}

static ssize_t pipe_write_ofd(ipo_ofd_t *ofd, const void *buf, size_t count) {
    ipo_pipe_t *p = ofd->pipe;
    if (!p) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&p->lock);
    if (p->readers == 0u) {
        pthread_mutex_unlock(&p->lock);
        errno = EPIPE;
        return -1;
    }
    if (pipe_ensure_space_locked(p, count) != 0) {
        pthread_mutex_unlock(&p->lock);
        errno = ENOMEM;
        return -1;
    }
    size_t first = p->cap - p->tail;
    if (first > count) first = count;
    memcpy(p->buf + p->tail, buf, first);
    if (count > first) {
        memcpy(p->buf, (const uint8_t *)buf + first, count - first);
    }
    p->tail = (p->tail + count) % p->cap;
    p->len += count;
    pthread_cond_broadcast(&p->can_read);
    pthread_mutex_unlock(&p->lock);
    return (ssize_t)count;
}

ssize_t read(int fd, void *buf, size_t count) {
    if (fd < 0 || (count > 0u && !buf)) {
        errno = (!buf && count > 0u) ? EFAULT : EBADF;
        return -1;
    }

    ipo_ofd_t *ofd = ofd_acquire_for_fd(fd);
    if (!ofd) {
        if (is_socket_fd(fd)) {
            if (count == 0u) return 0;
            return ipo_libc_socket_read ? ipo_libc_socket_read(fd, buf, count) : -1;
        }
        errno = EBADF;
        return -1;
    }
    if (count == 0u) {
        ofd_release(ofd);
        return 0;
    }

    ssize_t ret = -1;
    switch (ofd->kind) {
    case IPO_FD_CONSOLE_IN:
        ret = console_stdin_read(buf, count);
        break;
    case IPO_FD_DEVNULL:
        ret = 0;
        break;
    case IPO_FD_PIPE:
        if (ofd->pipe_is_writer) {
            errno = EBADF;
            ret = -1;
        } else {
            ret = pipe_read_ofd(ofd, buf, count);
        }
        break;
    case IPO_FD_FILE:
        if ((ofd->open_flags & IPO_O_ACCMODE) == O_WRONLY) {
            errno = EBADF;
            ret = -1;
        } else {
            pthread_mutex_lock(&ofd->io_lock);
            uint32_t args[3] = { (uint32_t)ofd->kfd, (uint32_t)(uintptr_t)buf, (uint32_t)count };
            int r = ipo_syscall(IPO_SYSCALL_FS_READ, 3u, args);
            pthread_mutex_unlock(&ofd->io_lock);
            if (r < 0) {
                errno = EIO;
                ret = -1;
            } else {
                ret = (ssize_t)r;
            }
        }
        break;
    default:
        errno = EBADF;
        ret = -1;
        break;
    }

    ofd_release(ofd);
    return ret;
}

ssize_t write(int fd, const void *buf, size_t count) {
    if (fd < 0 || (count > 0u && !buf)) {
        errno = (!buf && count > 0u) ? EFAULT : EBADF;
        return -1;
    }

    ipo_ofd_t *ofd = ofd_acquire_for_fd(fd);
    if (!ofd) {
        if (is_socket_fd(fd)) {
            if (count == 0u) return 0;
            return ipo_libc_socket_write ? ipo_libc_socket_write(fd, buf, count) : -1;
        }
        errno = EBADF;
        return -1;
    }
    if (count == 0u) {
        ofd_release(ofd);
        return 0;
    }

    ssize_t ret = -1;
    switch (ofd->kind) {
    case IPO_FD_CONSOLE_OUT:
    case IPO_FD_CONSOLE_ERR: {
        uint32_t args[2] = { (uint32_t)(uintptr_t)buf, (uint32_t)count };
        int r = ipo_syscall(IPO_SYSCALL_WRITE, 2u, args);
        if (r < 0) {
            errno = EIO;
            ret = -1;
        } else {
            ret = (ssize_t)r;
        }
        break;
    }
    case IPO_FD_DEVNULL:
        ret = (ssize_t)count;
        break;
    case IPO_FD_PIPE:
        if (!ofd->pipe_is_writer) {
            errno = EBADF;
            ret = -1;
        } else {
            ret = pipe_write_ofd(ofd, buf, count);
        }
        break;
    case IPO_FD_FILE:
        if ((ofd->open_flags & IPO_O_ACCMODE) == O_RDONLY) {
            errno = EBADF;
            ret = -1;
        } else {
            pthread_mutex_lock(&ofd->io_lock);
            if (ofd->open_flags & O_APPEND) {
                ipo_lseek64(ofd->kfd, 0, SEEK_END);
            }
            uint32_t args[3] = { (uint32_t)ofd->kfd, (uint32_t)(uintptr_t)buf, (uint32_t)count };
            int r = ipo_syscall(IPO_SYSCALL_FS_WRITE, 3u, args);
            pthread_mutex_unlock(&ofd->io_lock);
            if (r < 0) {
                errno = EIO;
                ret = -1;
            } else {
                ret = (ssize_t)r;
            }
        }
        break;
    default:
        errno = EBADF;
        ret = -1;
        break;
    }

    ofd_release(ofd);
    return ret;
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    if (fd < 0 || offset < 0 || (count > 0u && !buf)) {
        errno = (offset < 0) ? EINVAL : ((!buf && count > 0u) ? EFAULT : EBADF);
        return -1;
    }
    ipo_ofd_t *ofd = ofd_acquire_for_fd(fd);
    if (!ofd) {
        errno = EBADF;
        return -1;
    }
    if (ofd->kind != IPO_FD_FILE) {
        errno = (ofd->kind == IPO_FD_PIPE || ofd->kind <= IPO_FD_CONSOLE_ERR) ? ESPIPE : EINVAL;
        ofd_release(ofd);
        return -1;
    }
    if ((ofd->open_flags & IPO_O_ACCMODE) == O_WRONLY) {
        ofd_release(ofd);
        errno = EBADF;
        return -1;
    }
    if (count == 0u) {
        ofd_release(ofd);
        return 0;
    }

    pthread_mutex_lock(&ofd->io_lock);
    int64_t saved = ipo_lseek64(ofd->kfd, 0, SEEK_CUR);
    ssize_t ret = -1;
    if (ipo_lseek64(ofd->kfd, (int64_t)offset, SEEK_SET) < 0) {
        errno = EINVAL;
    } else {
        uint32_t args[3] = { (uint32_t)ofd->kfd, (uint32_t)(uintptr_t)buf, (uint32_t)count };
        int r = ipo_syscall(IPO_SYSCALL_FS_READ, 3u, args);
        if (r < 0) {
            errno = EIO;
        } else {
            ret = (ssize_t)r;
        }
    }
    if (saved >= 0) {
        ipo_lseek64(ofd->kfd, saved, SEEK_SET);
    }
    pthread_mutex_unlock(&ofd->io_lock);
    ofd_release(ofd);
    return ret;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    if (fd < 0 || offset < 0 || (count > 0u && !buf)) {
        errno = (offset < 0) ? EINVAL : ((!buf && count > 0u) ? EFAULT : EBADF);
        return -1;
    }
    ipo_ofd_t *ofd = ofd_acquire_for_fd(fd);
    if (!ofd) {
        errno = EBADF;
        return -1;
    }
    if (ofd->kind != IPO_FD_FILE) {
        errno = (ofd->kind == IPO_FD_PIPE || ofd->kind <= IPO_FD_CONSOLE_ERR) ? ESPIPE : EINVAL;
        ofd_release(ofd);
        return -1;
    }
    if ((ofd->open_flags & IPO_O_ACCMODE) == O_RDONLY) {
        ofd_release(ofd);
        errno = EBADF;
        return -1;
    }
    if (count == 0u) {
        ofd_release(ofd);
        return 0;
    }

    pthread_mutex_lock(&ofd->io_lock);
    int64_t saved = ipo_lseek64(ofd->kfd, 0, SEEK_CUR);
    ssize_t ret = -1;
    if (ipo_lseek64(ofd->kfd, (int64_t)offset, SEEK_SET) < 0) {
        errno = EINVAL;
    } else {
        uint32_t args[3] = { (uint32_t)ofd->kfd, (uint32_t)(uintptr_t)buf, (uint32_t)count };
        int r = ipo_syscall(IPO_SYSCALL_FS_WRITE, 3u, args);
        if (r < 0) {
            errno = EIO;
        } else {
            ret = (ssize_t)r;
        }
    }
    if (saved >= 0) {
        ipo_lseek64(ofd->kfd, saved, SEEK_SET);
    }
    pthread_mutex_unlock(&ofd->io_lock);
    ofd_release(ofd);
    return ret;
}

off_t lseek(int fd, off_t offset, int whence) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    ipo_ofd_t *ofd = ofd_acquire_for_fd(fd);
    if (!ofd) {
        errno = EBADF;
        return -1;
    }
    if (ofd->kind == IPO_FD_DEVNULL) {
        ofd_release(ofd);
        return 0;
    }
    if (ofd->kind != IPO_FD_FILE || ofd->kfd < 0) {
        ofd_release(ofd);
        errno = ESPIPE;
        return -1;
    }
    pthread_mutex_lock(&ofd->io_lock);
    int64_t res = ipo_lseek64(ofd->kfd, (int64_t)offset, whence);
    pthread_mutex_unlock(&ofd->io_lock);
    ofd_release(ofd);
    if (res < 0) {
        errno = EINVAL;
        return -1;
    }
    return (off_t)res;
}

int ipo_libc_fd_pipe(int pipefd[2]) {
    if (!pipefd) {
        errno = EFAULT;
        return -1;
    }
    ipo_pipe_t *p = (ipo_pipe_t *)calloc(1u, sizeof(ipo_pipe_t));
    if (!p) {
        errno = ENOMEM;
        return -1;
    }
    p->cap = 4096u;
    p->buf = (uint8_t *)malloc(p->cap);
    if (!p->buf) {
        free(p);
        errno = ENOMEM;
        return -1;
    }
    p->readers = 1u;
    p->writers = 1u;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->can_read, NULL);
    pthread_cond_init(&p->can_write, NULL);

    ipo_ofd_t *rofd = ofd_alloc(IPO_FD_PIPE, O_RDONLY);
    ipo_ofd_t *wofd = ofd_alloc(IPO_FD_PIPE, O_WRONLY);
    if (!rofd || !wofd) {
        if (rofd) { pthread_mutex_destroy(&rofd->io_lock); free(rofd); }
        if (wofd) { pthread_mutex_destroy(&wofd->io_lock); free(wofd); }
        pthread_cond_destroy(&p->can_read);
        pthread_cond_destroy(&p->can_write);
        pthread_mutex_destroy(&p->lock);
        free(p->buf);
        free(p);
        errno = ENOMEM;
        return -1;
    }
    rofd->pipe = p;
    rofd->pipe_is_writer = 0;
    wofd->pipe = p;
    wofd->pipe_is_writer = 1;

    pthread_mutex_lock(&s_fd_lock);
    int rfd = alloc_fd_slot_locked(0, -1);
    if (rfd >= 0) {
        s_fd_slots[rfd].ofd = rofd;
        s_fd_slots[rfd].fd_flags = 0;
    }
    int wfd = (rfd >= 0) ? alloc_fd_slot_locked(0, -1) : -1;
    if (wfd >= 0) {
        s_fd_slots[wfd].ofd = wofd;
        s_fd_slots[wfd].fd_flags = 0;
    } else if (rfd >= 0) {
        s_fd_slots[rfd].ofd = NULL;
        s_fd_slots[rfd].fd_flags = 0;
    }
    pthread_mutex_unlock(&s_fd_lock);

    if (rfd < 0 || wfd < 0) {
        ofd_release(rofd);
        ofd_release(wofd);
        errno = EMFILE;
        return -1;
    }
    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

static int dup_from_min_locked(int oldfd, int min_fd, int cloexec) {
    ensure_fd_init_locked();
    if (oldfd < 0 || min_fd < 0 || (size_t)oldfd >= s_fd_slots_cap || !s_fd_slots[oldfd].ofd) {
        errno = (min_fd < 0) ? EINVAL : EBADF;
        return -1;
    }
    int newfd = alloc_fd_slot_locked(min_fd, -1);
    if (newfd < 0) {
        errno = EMFILE;
        return -1;
    }
    ipo_ofd_t *ofd = s_fd_slots[oldfd].ofd;
    ofd_retain(ofd);
    s_fd_slots[newfd].ofd = ofd;
    s_fd_slots[newfd].fd_flags = cloexec ? FD_CLOEXEC : 0;
    return newfd;
}

int ipo_libc_fd_dup(int oldfd) {
    pthread_mutex_lock(&s_fd_lock);
    int res = dup_from_min_locked(oldfd, 0, 0);
    pthread_mutex_unlock(&s_fd_lock);
    return res;
}

int ipo_libc_fd_dup2(int oldfd, int newfd) {
    if (oldfd < 0 || newfd < 0) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&s_fd_lock);
    ensure_fd_init_locked();
    if ((size_t)oldfd >= s_fd_slots_cap || !s_fd_slots[oldfd].ofd) {
        pthread_mutex_unlock(&s_fd_lock);
        errno = EBADF;
        return -1;
    }
    if (oldfd == newfd) {
        pthread_mutex_unlock(&s_fd_lock);
        return newfd;
    }
    if (ensure_fd_cap_locked((size_t)newfd + 1u) != 0) {
        pthread_mutex_unlock(&s_fd_lock);
        errno = ENOMEM;
        return -1;
    }
    ipo_ofd_t *replaced = s_fd_slots[newfd].ofd;
    ipo_ofd_t *src = s_fd_slots[oldfd].ofd;
    ofd_retain(src);
    s_fd_slots[newfd].ofd = src;
    s_fd_slots[newfd].fd_flags = 0;
    pthread_mutex_unlock(&s_fd_lock);

    if (replaced) {
        ofd_release(replaced);
    }
    return newfd;
}

int ipo_libc_fd_fcntl(int fd, int cmd, long arg) {
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&s_fd_lock);
    ensure_fd_init_locked();
    if ((size_t)fd >= s_fd_slots_cap || !s_fd_slots[fd].ofd) {
        pthread_mutex_unlock(&s_fd_lock);
        errno = EBADF;
        return -1;
    }
    int res = -1;
    switch (cmd) {
    case F_DUPFD:
        res = dup_from_min_locked(fd, (int)arg, 0);
        break;
    case F_DUPFD_CLOEXEC:
        res = dup_from_min_locked(fd, (int)arg, 1);
        break;
    case F_GETFD:
        res = s_fd_slots[fd].fd_flags;
        break;
    case F_SETFD:
        s_fd_slots[fd].fd_flags = (int)(arg & FD_CLOEXEC);
        res = 0;
        break;
    case F_GETFL:
        res = s_fd_slots[fd].ofd->open_flags;
        break;
    case F_SETFL: {
        int mask = O_APPEND | O_NONBLOCK;
        s_fd_slots[fd].ofd->open_flags =
            (s_fd_slots[fd].ofd->open_flags & ~mask) | ((int)arg & mask);
        res = 0;
        break;
    }
    default:
        errno = EINVAL;
        res = -1;
        break;
    }
    pthread_mutex_unlock(&s_fd_lock);
    return res;
}

static short fd_poll_ready_mask(int fd) {
    if (fd < 0) return 0;
    ipo_ofd_t *ofd = ofd_acquire_for_fd(fd);
    if (!ofd) {
        return is_socket_fd(fd) ? (POLLIN | POLLOUT) : POLLNVAL;
    }
    short mask = 0;
    switch (ofd->kind) {
    case IPO_FD_FILE:
    case IPO_FD_DEVNULL:
        mask = POLLIN | POLLOUT;
        break;
    case IPO_FD_CONSOLE_IN:
        pthread_mutex_lock(&s_stdin_lock);
        if (stdin_line_pos < stdin_line_len) mask |= POLLIN;
        pthread_mutex_unlock(&s_stdin_lock);
        break;
    case IPO_FD_CONSOLE_OUT:
    case IPO_FD_CONSOLE_ERR:
        mask = POLLOUT;
        break;
    case IPO_FD_PIPE:
        if (ofd->pipe) {
            ipo_pipe_t *p = ofd->pipe;
            pthread_mutex_lock(&p->lock);
            if (!ofd->pipe_is_writer) {
                if (p->len > 0u) mask |= POLLIN;
                if (p->writers == 0u) mask |= POLLHUP;
            } else {
                if (p->readers == 0u) mask |= POLLERR | POLLHUP;
                else mask |= POLLOUT;
            }
            pthread_mutex_unlock(&p->lock);
        }
        break;
    default:
        mask = POLLNVAL;
        break;
    }
    ofd_release(ofd);
    return mask;
}

int ipo_libc_fd_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (!fds && nfds > 0u) {
        errno = EFAULT;
        return -1;
    }
    struct timespec start_ts, now_ts;
    if (timeout > 0) {
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
    }

    while (1) {
        int ready = 0;
        for (nfds_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            short avail = fd_poll_ready_mask(fds[i].fd);
            short rev = (short)((avail & (fds[i].events & (POLLIN | POLLPRI | POLLOUT))) |
                                (avail & (POLLERR | POLLHUP | POLLNVAL)));
            fds[i].revents = rev;
            if (rev != 0) ready++;
        }
        if (ready > 0 || timeout == 0) {
            return ready;
        }
        if (timeout > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            int64_t elapsed_ms = ((int64_t)now_ts.tv_sec - (int64_t)start_ts.tv_sec) * 1000LL +
                                 ((int64_t)now_ts.tv_nsec - (int64_t)start_ts.tv_nsec) / 1000000LL;
            if (elapsed_ms >= (int64_t)timeout) {
                return 0;
            }
        }
        struct timespec req = { 0, 1000000L };
        nanosleep(&req, NULL);
    }
}

int ipo_libc_fd_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                       struct timeval *timeout) {
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    int timeout_ms = -1;
    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_usec < 0 || timeout->tv_usec >= 1000000L) {
            errno = EINVAL;
            return -1;
        }
        int64_t ms = (int64_t)timeout->tv_sec * 1000LL + (int64_t)timeout->tv_usec / 1000LL;
        if (ms == 0 && timeout->tv_usec > 0) ms = 1;
        timeout_ms = (ms > 0x7FFFFFFFLL) ? 0x7FFFFFFF : (int)ms;
    }

    struct timespec start_ts, now_ts;
    if (timeout_ms > 0) {
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
    }

    fd_set in_r, in_w, in_e;
    if (readfds) memcpy(&in_r, readfds, sizeof(fd_set));
    if (writefds) memcpy(&in_w, writefds, sizeof(fd_set));
    if (exceptfds) memcpy(&in_e, exceptfds, sizeof(fd_set));

    while (1) {
        fd_set out_r, out_w, out_e;
        FD_ZERO(&out_r);
        FD_ZERO(&out_w);
        FD_ZERO(&out_e);
        int count = 0;

        for (int fd = 0; fd < nfds; fd++) {
            int want_r = readfds && FD_ISSET(fd, &in_r);
            int want_w = writefds && FD_ISSET(fd, &in_w);
            int want_e = exceptfds && FD_ISSET(fd, &in_e);
            if (!want_r && !want_w && !want_e) continue;

            short avail = fd_poll_ready_mask(fd);
            if (avail & POLLNVAL) {
                errno = EBADF;
                return -1;
            }
            if (want_r && (avail & (POLLIN | POLLHUP | POLLERR))) {
                FD_SET(fd, &out_r);
                count++;
            }
            if (want_w && (avail & (POLLOUT | POLLERR | POLLHUP))) {
                FD_SET(fd, &out_w);
                count++;
            }
            if (want_e && (avail & POLLERR)) {
                FD_SET(fd, &out_e);
                count++;
            }
        }

        if (count > 0 || timeout_ms == 0) {
            if (readfds) memcpy(readfds, &out_r, sizeof(fd_set));
            if (writefds) memcpy(writefds, &out_w, sizeof(fd_set));
            if (exceptfds) memcpy(exceptfds, &out_e, sizeof(fd_set));
            return count;
        }
        if (timeout_ms > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            int64_t elapsed_ms = ((int64_t)now_ts.tv_sec - (int64_t)start_ts.tv_sec) * 1000LL +
                                 ((int64_t)now_ts.tv_nsec - (int64_t)start_ts.tv_nsec) / 1000000LL;
            if (elapsed_ms >= (int64_t)timeout_ms) {
                if (readfds) FD_ZERO(readfds);
                if (writefds) FD_ZERO(writefds);
                if (exceptfds) FD_ZERO(exceptfds);
                return 0;
            }
        }
        struct timespec req = { 0, 1000000L };
        nanosleep(&req, NULL);
    }
}

int unlink(const char *pathname) {
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }
    if (pathname[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) != 0) {
        errno = ipo_libc_parent_dir_errno(pathname);
        return -1;
    }
    if (st.mode & IPO_INODE_TYPE_DIR) {
        errno = EISDIR;
        return -1;
    }
    size_t plen = strlen(pathname);
    if (plen > 1 && pathname[plen - 1] == '/') {
        errno = ENOTDIR;
        return -1;
    }
    if (st.mode & IPO_INODE_FLAG_PROTECTED) {
        errno = EACCES;
        return -1;
    }
    if (ipo_delete(pathname) == 0) {
        return 0;
    }
    errno = EIO;
    return -1;
}

int rmdir(const char *pathname) {
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }
    if (pathname[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (strcmp(pathname, "/dev/null") == 0) {
        return 0;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) != 0) {
        errno = ipo_libc_parent_dir_errno(pathname);
        return -1;
    }
    if (!(st.mode & IPO_INODE_TYPE_DIR)) {
        errno = ENOTDIR;
        return -1;
    }
    if (st.mode & IPO_INODE_FLAG_PROTECTED) {
        errno = EACCES;
        return -1;
    }
    if (ipo_rmdir(pathname) == 0) {
        return 0;
    }
    errno = ENOTEMPTY;
    return -1;
}

char *getcwd(char *buf, size_t size) {
    if (buf != NULL && size == 0u) {
        errno = EINVAL;
        return NULL;
    }
    size_t cap = (size > 256u) ? size : 256u;
    char *tmp = (char *)malloc(cap);
    if (!tmp) {
        errno = ENOMEM;
        return NULL;
    }
    while (1) {
        if (ipo_getcwd(tmp, (uint32_t)cap) == 0) {
            free(tmp);
            errno = EIO;
            return NULL;
        }
        size_t len = strlen(tmp);
        if (len + 1u < cap) {
            break;
        }
        if (cap > ((size_t)-1) / 2u) {
            free(tmp);
            errno = ENOMEM;
            return NULL;
        }
        size_t ncap = cap * 2u;
        char *nt = (char *)realloc(tmp, ncap);
        if (!nt) {
            free(tmp);
            errno = ENOMEM;
            return NULL;
        }
        tmp = nt;
        cap = ncap;
    }

    size_t need = strlen(tmp) + 1u;
    if (buf == NULL) {
        if (size > 0u && need > size) {
            free(tmp);
            errno = ERANGE;
            return NULL;
        }
        size_t alloc_sz = size ? size : need;
        char *shrunk = (char *)realloc(tmp, alloc_sz);
        return shrunk ? shrunk : tmp;
    }

    if (need > size) {
        free(tmp);
        errno = ERANGE;
        return NULL;
    }
    memcpy(buf, tmp, need);
    free(tmp);
    return buf;
}

int chdir(const char *path) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (path[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    struct ipo_inode st;
    if (ipo_stat(path, &st) != 0) {
        errno = ipo_libc_parent_dir_errno(path);
        return -1;
    }
    if (!(st.mode & IPO_INODE_TYPE_DIR)) {
        errno = ENOTDIR;
        return -1;
    }
    if (ipo_chdir(path) != 0) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

int access(const char *pathname, int mode) {
    if (!pathname) {
        errno = EINVAL;
        return -1;
    }
    if ((mode & ~(F_OK | R_OK | W_OK | X_OK)) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (pathname[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    if (strcmp(pathname, "/dev/null") == 0) {
        return 0;
    }
    size_t plen = strlen(pathname);
    int trailing_slash = (plen > 1 && pathname[plen - 1] == '/');
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) != 0) {
        errno = ipo_libc_parent_dir_errno(pathname);
        return -1;
    }
    if (trailing_slash && !(st.mode & IPO_INODE_TYPE_DIR)) {
        errno = ENOTDIR;
        return -1;
    }
    if ((mode & W_OK) && (st.mode & IPO_INODE_FLAG_PROTECTED)) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int isatty(int fd) {
    ipo_ofd_t *ofd = ofd_acquire_for_fd(fd);
    if (!ofd) {
        errno = EBADF;
        return 0;
    }
    int tty = (ofd->kind == IPO_FD_CONSOLE_IN ||
               ofd->kind == IPO_FD_CONSOLE_OUT ||
               ofd->kind == IPO_FD_CONSOLE_ERR);
    ofd_release(ofd);
    if (!tty) errno = EINVAL;
    return tty ? 1 : 0;
}

pid_t getpid(void) {
    return (pid_t)ipo_syscall(IPO_SYSCALL_GETPID, 0u, NULL);
}

pid_t getppid(void) {
    return 1;
}

uid_t getuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

void *sbrk(intptr_t increment) {
    void *p = ipo_sbrk((int32_t)increment);
    if (p == (void *)-1) {
        errno = ENOMEM;
    }
    return p;
}

int brk(void *addr) {
    if (!addr) {
        errno = EINVAL;
        return -1;
    }
    void *current_brk = sbrk(0);
    if (current_brk == (void *)-1) {
        return -1;
    }
    intptr_t inc = (char *)addr - (char *)current_brk;
    if (inc == 0) return 0;
    void *res = sbrk(inc);
    if (res == (void *)-1) {
        return -1;
    }
    return 0;
}

unsigned int sleep(unsigned int seconds) {
    if (seconds == 0u) return 0u;
    struct timespec req = { (long)seconds, 0L };
    struct timespec rem = { 0L, 0L };
    if (nanosleep(&req, &rem) < 0) {
        return (unsigned int)(rem.tv_sec > 0 ? rem.tv_sec : 0);
    }
    return 0u;
}

int usleep(useconds_t usec) {
    if (usec == 0u) return 0;
    struct timespec req = {
        (long)(usec / 1000000u),
        (long)((usec % 1000000u) * 1000u)
    };
    return nanosleep(&req, NULL);
}

int execv(const char *path, char *const argv[]) {
    if (!path || !path[0]) {
        errno = ENOENT;
        return -1;
    }
    int argc = 0;
    if (argv) {
        while (argv[argc]) argc++;
    }
    int pid = ipo_exec(path, argc, (char **)argv);
    if (pid < 0) {
        errno = ENOENT;
        return -1;
    }
    exit(ipo_get_exit_code());
    return 0;
}

int execvp(const char *file, char *const argv[]) {
    if (!file || !file[0]) {
        errno = ENOENT;
        return -1;
    }
    if (strchr(file, '/') != NULL) {
        return execv(file, argv);
    }
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0]) {
        path_env = "/applications:/bin:/usr/bin";
    }

    size_t flen = strlen(file);
    const char *p = path_env;
    while (*p) {
        const char *colon = strchr(p, ':');
        size_t dlen = colon ? (size_t)(colon - p) : strlen(p);
        const char *dir = (dlen == 0u) ? "." : p;
        size_t eff_dlen = (dlen == 0u) ? 1u : dlen;

        char *cand = (char *)malloc(eff_dlen + 1u + flen + 1u);
        if (!cand) {
            errno = ENOMEM;
            return -1;
        }
        memcpy(cand, dir, eff_dlen);
        size_t pos = eff_dlen;
        if (cand[pos - 1u] != '/') {
            cand[pos++] = '/';
        }
        memcpy(cand + pos, file, flen + 1u);

        struct ipo_inode st;
        if (ipo_stat(cand, &st) == 0 && !(st.mode & IPO_INODE_TYPE_DIR)) {
            execv(cand, argv);
            free(cand);
            return -1;
        }
        free(cand);
        if (!colon) break;
        p = colon + 1;
    }

    return execv(file, argv);
}

static int exec_collect_va(const char *arg0, va_list ap, char ***out_argv) {
    size_t cap = 16u;
    size_t count = 0u;
    char **argv = (char **)malloc(cap * sizeof(char *));
    if (!argv) return -1;
    argv[count++] = (char *)arg0;
    const char *cur = arg0;
    while (cur != NULL) {
        cur = va_arg(ap, const char *);
        if (count >= cap) {
            size_t ncap = cap * 2u;
            char **na = (char **)realloc(argv, ncap * sizeof(char *));
            if (!na) {
                free(argv);
                return -1;
            }
            argv = na;
            cap = ncap;
        }
        argv[count++] = (char *)cur;
    }
    *out_argv = argv;
    return 0;
}

int execl(const char *path, const char *arg, ...) {
    char **argv = NULL;
    va_list ap;
    va_start(ap, arg);
    int ok = exec_collect_va(arg, ap, &argv);
    va_end(ap);
    if (ok != 0) {
        errno = ENOMEM;
        return -1;
    }
    int r = execv(path, argv);
    free(argv);
    return r;
}

int execlp(const char *file, const char *arg, ...) {
    char **argv = NULL;
    va_list ap;
    va_start(ap, arg);
    int ok = exec_collect_va(arg, ap, &argv);
    va_end(ap);
    if (ok != 0) {
        errno = ENOMEM;
        return -1;
    }
    int r = execvp(file, argv);
    free(argv);
    return r;
}

pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    (void)buf;
    (void)bufsiz;
    if (!pathname) {
        errno = EFAULT;
        return -1;
    }
    struct ipo_inode st;
    if (ipo_stat(pathname, &st) != 0) {
        errno = ENOENT;
        return -1;
    }
    errno = EINVAL;
    return -1;
}

int symlink(const char *target, const char *linkpath) {
    (void)target;
    (void)linkpath;
    errno = ENOSYS;
    return -1;
}
