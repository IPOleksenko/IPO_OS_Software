#define USE_THE_REPOSITORY_VARIABLE

#include "git-compat-util.h"
#include "run-command.h"
#include "repository.h"
#include "setup.h"
#include "builtin.h"
#include "strbuf.h"
#include "strvec.h"
#include "environment.h"

#include <syscall.h>
#include <termios.h>
#include <sys/statvfs.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <netinet/in.h>

#undef exit
#undef strcpy
#undef strncpy
#undef strcat
#undef strncat
#undef sprintf
#undef vsprintf

/* Real libc functions wrapped via linker --wrap */
extern ssize_t __real_read(int fd, void *buf, size_t count);
extern ssize_t __real_write(int fd, const void *buf, size_t count);
extern int __real_close(int fd);
extern off_t __real_lseek(int fd, off_t offset, int whence);
extern int __real_fstat(int fd, struct stat *statbuf);
extern int __real_isatty(int fd);
extern void __real_exit(int status) __attribute__((noreturn));
extern void __real__exit(int status) __attribute__((noreturn));
extern int __real_main(int argc, const char **argv);

extern int cmd_remote_curl(int argc, const char **argv);

/* --------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */
ssize_t __wrap_read(int fd, void *buf, size_t count);
ssize_t __wrap_write(int fd, const void *buf, size_t count);
int __wrap_close(int fd);
off_t __wrap_lseek(int fd, off_t offset, int whence);
int __wrap_fstat(int fd, struct stat *statbuf);
int __wrap_isatty(int fd);
void __wrap_exit(int status) __attribute__((noreturn));
void __wrap__exit(int status) __attribute__((noreturn));

/* --------------------------------------------------------------------------
 * Cooperative User-Space Fibers (x86-32)
 * -------------------------------------------------------------------------- */
#define FIBER_STACK_SZ (2 * 1024 * 1024)

#define PIPE_FD_BASE   64
#define DEVNULL_FD     0x3FFFFFFF

typedef int (*fiber_cmd_fn_t)(int argc, const char **argv);
typedef int (*fiber_async_fn_t)(int in, int out, void *data);

typedef struct {
    int id;
    int in_use;
    int started;
    int finished;
    int is_async;
    int exit_code;
    int parent_fiber_id;
    int waiting_on_pipe;

    uint32_t esp;
    unsigned char *stack_base;

    int *fd_map;
    int fd_map_cap;
    int owns_stdio_fds;
    FILE saved_stdin;
    FILE saved_stdout;

    /* Command payload */
    int kind; /* 1=cmd_remote_curl, 2=cmd_fetch_pack, 3=cmd_send_pack, 4=async */
    int argc;
    char **argv;

    fiber_async_fn_t async_fn;
    int async_in;
    int async_out;
    void *async_data;
} ipo_fiber_t;

static ipo_fiber_t *s_fibers = NULL;
static int s_fibers_cap = 0;
static int s_current_fiber = 0;
static int s_fibers_initialized = 0;

static int fiber_ensure_vfd(ipo_fiber_t *f, int fd) {
    if (!f || fd < 0) return -1;
    if (fd < f->fd_map_cap) return 0;
    int ncap = f->fd_map_cap > 0 ? f->fd_map_cap : 32;
    while (ncap <= fd) {
        if (ncap > INT32_MAX / 2) { ncap = fd + 1; break; }
        ncap *= 2;
    }
    if ((size_t)ncap > SIZE_MAX / sizeof(int)) return -1;
    int *nm = (int *)realloc(f->fd_map, (size_t)ncap * sizeof(int));
    if (!nm) return -1;
    for (int i = f->fd_map_cap; i < ncap; i++) nm[i] = -1;
    f->fd_map = nm;
    f->fd_map_cap = ncap;
    return 0;
}

static int ensure_fibers_cap(int min_cap) {
    if (min_cap <= s_fibers_cap) return 0;
    int ncap = s_fibers_cap > 0 ? s_fibers_cap : 16;
    while (ncap < min_cap) {
        if (ncap > INT32_MAX / 2) { ncap = min_cap; break; }
        ncap *= 2;
    }
    if ((size_t)ncap > SIZE_MAX / sizeof(ipo_fiber_t)) return -1;
    ipo_fiber_t *nf = (ipo_fiber_t *)realloc(s_fibers, (size_t)ncap * sizeof(ipo_fiber_t));
    if (!nf) return -1;
    for (int i = s_fibers_cap; i < ncap; i++) {
        memset(&nf[i], 0, sizeof(ipo_fiber_t));
        nf[i].id = i;
        nf[i].parent_fiber_id = -1;
        nf[i].waiting_on_pipe = -1;
    }
    s_fibers = nf;
    s_fibers_cap = ncap;
    return 0;
}

extern void ipo_fiber_switch(uint32_t *old_esp, uint32_t new_esp);

__asm__(
    ".text\n"
    ".global ipo_fiber_switch\n"
    "ipo_fiber_switch:\n"
    "    movl 4(%esp), %eax\n"
    "    movl 8(%esp), %edx\n"
    "    pushl %ebp\n"
    "    pushl %ebx\n"
    "    pushl %esi\n"
    "    pushl %edi\n"
    "    movl %esp, (%eax)\n"
    "    movl %edx, %esp\n"
    "    popl %edi\n"
    "    popl %esi\n"
    "    popl %ebx\n"
    "    popl %ebp\n"
    "    ret\n"
);

static void init_fiber_subsystem(void) {
    if (s_fibers_initialized) return;
    if (ensure_fibers_cap(16) < 0) return;
    s_fibers[0].in_use = 1;
    s_fibers[0].started = 1;
    s_fibers[0].saved_stdin = *stdin;
    s_fibers[0].saved_stdout = *stdout;
    s_current_fiber = 0;
    s_fibers_initialized = 1;
}

static void fiber_switch_to(int next_id) {
    init_fiber_subsystem();
    if (next_id < 0 || next_id >= s_fibers_cap || next_id == s_current_fiber) return;
    if (!s_fibers[next_id].in_use || s_fibers[next_id].finished) return;

    int cur_id = s_current_fiber;
    s_fibers[cur_id].saved_stdin = *stdin;
    s_fibers[cur_id].saved_stdout = *stdout;

    s_current_fiber = next_id;
    *stdin = s_fibers[next_id].saved_stdin;
    *stdout = s_fibers[next_id].saved_stdout;

    ipo_fiber_switch(&s_fibers[cur_id].esp, s_fibers[next_id].esp);
}

/* --------------------------------------------------------------------------
 * Dynamic In-Memory Pipe Table & Virtual FD Mapper
 * -------------------------------------------------------------------------- */
typedef struct {
    unsigned char *buf;
    size_t head;
    size_t len;
    size_t cap;
    int reader_refs;
    int writer_refs;
    int in_use;
    int writer_fiber_id;
    int reader_fiber_id;
    int pending_child_idx;
} ipo_pipe_t;

static ipo_pipe_t *s_pipes = NULL;
static int s_pipes_cap = 0;

static int ensure_pipes_cap(int min_cap) {
    if (min_cap <= s_pipes_cap) return 0;
    int ncap = s_pipes_cap > 0 ? s_pipes_cap : 64;
    while (ncap < min_cap) {
        if (ncap > INT32_MAX / 2) { ncap = min_cap; break; }
        ncap *= 2;
    }
    if ((size_t)ncap > SIZE_MAX / sizeof(ipo_pipe_t)) return -1;
    ipo_pipe_t *np = (ipo_pipe_t *)realloc(s_pipes, (size_t)ncap * sizeof(ipo_pipe_t));
    if (!np) return -1;
    memset(&np[s_pipes_cap], 0, (size_t)(ncap - s_pipes_cap) * sizeof(ipo_pipe_t));
    s_pipes = np;
    s_pipes_cap = ncap;
    return 0;
}

static int is_pipe_fd(int fd) {
    if (fd < PIPE_FD_BASE || fd == DEVNULL_FD) return 0;
    int idx = (fd - PIPE_FD_BASE) / 2;
    return idx >= 0 && idx < s_pipes_cap;
}

static int pipe_index_of(int fd) {
    return (fd - PIPE_FD_BASE) / 2;
}

static int pipe_is_writer(int fd) {
    return ((fd - PIPE_FD_BASE) & 1) != 0;
}

static int resolve_vfd(int fd) {
    init_fiber_subsystem();
    ipo_fiber_t *f = &s_fibers[s_current_fiber];
    if (fd >= 0 && fd < f->fd_map_cap && f->fd_map) {
        int mapped = f->fd_map[fd];
        if (mapped >= 0) return mapped;
    }
    return fd;
}

static void run_pending_child_for_pipe(int pipe_idx);

static int pick_runnable_fiber_for_pipe(int pipe_idx) {
    int *visited = (int *)calloc((size_t)(s_fibers_cap > 0 ? s_fibers_cap : 1), sizeof(int));
    int cur_pipe = pipe_idx;

    while (cur_pipe >= 0 && cur_pipe < s_pipes_cap && s_pipes[cur_pipe].in_use) {
        int wf = s_pipes[cur_pipe].writer_fiber_id;
        if (wf < 0 || wf >= s_fibers_cap || (visited && visited[wf])) break;
        if (visited) visited[wf] = 1;
        if (!s_fibers[wf].in_use || s_fibers[wf].finished) break;

        int wait_p = s_fibers[wf].waiting_on_pipe;
        if (wait_p >= 0 && wait_p < s_pipes_cap && s_pipes[wait_p].in_use &&
            s_pipes[wait_p].head >= s_pipes[wait_p].len &&
            s_pipes[wait_p].writer_refs > 0) {
            cur_pipe = wait_p;
            continue;
        }
        free(visited);
        return wf;
    }
    free(visited);

    /* Fallback: any runnable fiber that is either not waiting on a pipe or whose pipe now has data/EOF */
    for (int i = s_fibers_cap - 1; i >= 0; i--) {
        if (i == s_current_fiber) continue;
        if (!s_fibers[i].in_use || s_fibers[i].finished) continue;
        int wp = s_fibers[i].waiting_on_pipe;
        if (wp < 0) return i;
        if (wp >= 0 && wp < s_pipes_cap &&
            (s_pipes[wp].head < s_pipes[wp].len || s_pipes[wp].writer_refs <= 0)) {
            return i;
        }
    }

    int pf = s_fibers[s_current_fiber].parent_fiber_id;
    if (pf >= 0 && pf != s_current_fiber && pf < s_fibers_cap &&
        s_fibers[pf].in_use && !s_fibers[pf].finished) {
        return pf;
    }
    return (s_current_fiber != 0) ? 0 : -1;
}

static void drain_pipe_from_writer_fiber(int rfd) {
    int actual = resolve_vfd(rfd);
    if (!is_pipe_fd(actual) || pipe_is_writer(actual)) return;
    int pidx = pipe_index_of(actual);
    ipo_pipe_t *p = &s_pipes[pidx];
    while (p->in_use && p->writer_refs > 0 && p->writer_fiber_id >= 0 &&
           !s_fibers[p->writer_fiber_id].finished) {
        s_fibers[s_current_fiber].waiting_on_pipe = pidx;
        p->reader_fiber_id = s_current_fiber;
        int next = pick_runnable_fiber_for_pipe(pidx);
        if (next < 0 || next == s_current_fiber) break;
        fiber_switch_to(next);
    }
    s_fibers[s_current_fiber].waiting_on_pipe = -1;
}

int pipe(int pipefd[2]) {
    init_fiber_subsystem();
    if (!pipefd) {
        errno = EFAULT;
        return -1;
    }
    int slot = -1;
    for (int i = 0; i < s_pipes_cap; i++) {
        if (!s_pipes[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        int old_cap = s_pipes_cap;
        if (ensure_pipes_cap(old_cap + 1) < 0) {
            errno = EMFILE;
            return -1;
        }
        slot = old_cap;
    }
    memset(&s_pipes[slot], 0, sizeof(ipo_pipe_t));
    s_pipes[slot].in_use = 1;
    s_pipes[slot].reader_refs = 1;
    s_pipes[slot].writer_refs = 1;
    s_pipes[slot].writer_fiber_id = -1;
    s_pipes[slot].reader_fiber_id = -1;
    s_pipes[slot].pending_child_idx = -1;
    pipefd[0] = PIPE_FD_BASE + slot * 2;
    pipefd[1] = PIPE_FD_BASE + slot * 2 + 1;
    return 0;
}

static void pipe_inc_ref(int actual_fd) {
    if (!is_pipe_fd(actual_fd)) return;
    int idx = pipe_index_of(actual_fd);
    if (!s_pipes[idx].in_use) return;
    if (pipe_is_writer(actual_fd)) s_pipes[idx].writer_refs++;
    else s_pipes[idx].reader_refs++;
}

static int raw_close_actual_fd(int actual_fd) {
    if (actual_fd == DEVNULL_FD) return 0;
    if (is_pipe_fd(actual_fd)) {
        int idx = pipe_index_of(actual_fd);
        ipo_pipe_t *p = &s_pipes[idx];
        if (!p->in_use) {
            errno = EBADF;
            return -1;
        }
        if (pipe_is_writer(actual_fd)) {
            if (p->writer_refs > 0) p->writer_refs--;
            if (p->writer_refs == 0 && p->pending_child_idx >= 0) {
                run_pending_child_for_pipe(idx);
            }
        } else {
            if (p->reader_refs > 0) p->reader_refs--;
        }
        if (p->reader_refs <= 0 && p->writer_refs <= 0) {
            free(p->buf);
            memset(p, 0, sizeof(*p));
            p->writer_fiber_id = -1;
            p->reader_fiber_id = -1;
            p->pending_child_idx = -1;
        }
        return 0;
    }
    return __real_close(actual_fd);
}

ssize_t __wrap_read(int fd, void *buf, size_t count) {
    if (fd < 0 || (!buf && count > 0)) {
        errno = EBADF;
        return -1;
    }
    if (count == 0) return 0;

    int actual_fd = resolve_vfd(fd);
    if (actual_fd == DEVNULL_FD) {
        return 0;
    }
    if (is_pipe_fd(actual_fd)) {
        if (pipe_is_writer(actual_fd)) {
            errno = EBADF;
            return -1;
        }
        int idx = pipe_index_of(actual_fd);
        ipo_pipe_t *p = &s_pipes[idx];
        if (!p->in_use) {
            errno = EBADF;
            return -1;
        }
        p->reader_fiber_id = s_current_fiber;

        while (p->head >= p->len) {
            if (p->writer_refs <= 0) {
                s_fibers[s_current_fiber].waiting_on_pipe = -1;
                return 0; /* EOF */
            }
            int next_f = pick_runnable_fiber_for_pipe(idx);
            if (next_f < 0 || next_f == s_current_fiber) {
                s_fibers[s_current_fiber].waiting_on_pipe = -1;
                return 0;
            }
            s_fibers[s_current_fiber].waiting_on_pipe = idx;
            fiber_switch_to(next_f);
        }
        s_fibers[s_current_fiber].waiting_on_pipe = -1;

        size_t avail = p->len - p->head;
        size_t to_read = count < avail ? count : avail;
        memcpy(buf, p->buf + p->head, to_read);
        p->head += to_read;
        if (p->head >= p->len) {
            p->head = 0;
            p->len = 0;
        }
        return (ssize_t)to_read;
    }
    return __real_read(actual_fd, buf, count);
}

ssize_t __wrap_write(int fd, const void *buf, size_t count) {
    if (fd < 0 || (!buf && count > 0)) {
        errno = EBADF;
        return -1;
    }
    if (count == 0) return 0;

    int actual_fd = resolve_vfd(fd);
    if (actual_fd == DEVNULL_FD) {
        return (ssize_t)count;
    }
    if (is_pipe_fd(actual_fd)) {
        if (!pipe_is_writer(actual_fd)) {
            errno = EBADF;
            return -1;
        }
        int idx = pipe_index_of(actual_fd);
        ipo_pipe_t *p = &s_pipes[idx];
        if (!p->in_use || p->reader_refs <= 0) {
            errno = EPIPE;
            return -1;
        }
        if (p->head > 0 && p->head == p->len) {
            p->head = 0;
            p->len = 0;
        } else if (p->head >= 65536 && p->head * 2 >= p->len) {
            size_t rem = p->len - p->head;
            memmove(p->buf, p->buf + p->head, rem);
            p->head = 0;
            p->len = rem;
        }
        size_t needed = p->len + count;
        if (needed > p->cap) {
            size_t new_cap = p->cap ? p->cap * 2 : 8192;
            while (new_cap < needed) new_cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(p->buf, new_cap);
            if (!nb) {
                errno = ENOMEM;
                return -1;
            }
            p->buf = nb;
            p->cap = new_cap;
        }
        memcpy(p->buf + p->len, buf, count);
        p->len += count;
        return (ssize_t)count;
    }
    return __real_write(actual_fd, buf, count);
}

int __wrap_close(int fd) {
    init_fiber_subsystem();
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    ipo_fiber_t *f = &s_fibers[s_current_fiber];
    if (fd < f->fd_map_cap && f->fd_map && f->fd_map[fd] >= 0) {
        int actual = f->fd_map[fd];
        f->fd_map[fd] = -1;
        return raw_close_actual_fd(actual);
    }
    return raw_close_actual_fd(fd);
}

off_t __wrap_lseek(int fd, off_t offset, int whence) {
    int actual_fd = resolve_vfd(fd);
    if (is_pipe_fd(actual_fd)) {
        errno = ESPIPE;
        return (off_t)-1;
    }
    if (actual_fd == DEVNULL_FD) {
        return 0;
    }
    return __real_lseek(actual_fd, offset, whence);
}

int __wrap_fstat(int fd, struct stat *statbuf) {
    if (!statbuf) {
        errno = EFAULT;
        return -1;
    }
    int actual_fd = resolve_vfd(fd);
    if (is_pipe_fd(actual_fd)) {
        memset(statbuf, 0, sizeof(*statbuf));
        statbuf->st_mode = S_IFIFO | 0600;
        statbuf->st_nlink = 1;
        return 0;
    }
    if (actual_fd == DEVNULL_FD) {
        memset(statbuf, 0, sizeof(*statbuf));
        statbuf->st_mode = S_IFCHR | 0666;
        statbuf->st_nlink = 1;
        return 0;
    }
    return __real_fstat(actual_fd, statbuf);
}

int __wrap_isatty(int fd) {
    int actual_fd = resolve_vfd(fd);
    if (is_pipe_fd(actual_fd) || actual_fd == DEVNULL_FD) {
        return 0;
    }
    return __real_isatty(actual_fd);
}

int dup(int oldfd) {
    init_fiber_subsystem();
    int actual = resolve_vfd(oldfd);
    if (actual < 0) {
        errno = EBADF;
        return -1;
    }
    if (is_pipe_fd(actual)) {
        pipe_inc_ref(actual);
        return actual;
    }
    if (actual == DEVNULL_FD) {
        return DEVNULL_FD;
    }
    ipo_fiber_t *f = &s_fibers[s_current_fiber];
    for (int v = 3; v < f->fd_map_cap; v++) {
        if (f->fd_map && f->fd_map[v] < 0) {
            f->fd_map[v] = actual;
            return v;
        }
    }
    int new_v = f->fd_map_cap > 3 ? f->fd_map_cap : 3;
    if (fiber_ensure_vfd(f, new_v) == 0) {
        f->fd_map[new_v] = actual;
        return new_v;
    }
    return actual;
}

int dup2(int oldfd, int newfd) {
    init_fiber_subsystem();
    if (oldfd < 0 || newfd < 0) {
        errno = EBADF;
        return -1;
    }
    ipo_fiber_t *f = &s_fibers[s_current_fiber];
    if (fiber_ensure_vfd(f, newfd) < 0) {
        errno = ENOMEM;
        return -1;
    }
    int actual_old = resolve_vfd(oldfd);
    int actual_new = resolve_vfd(newfd);
    if (actual_old == actual_new && oldfd == newfd) {
        return newfd;
    }
    if (f->fd_map[newfd] >= 0) {
        raw_close_actual_fd(f->fd_map[newfd]);
        f->fd_map[newfd] = -1;
    }
    if (is_pipe_fd(actual_old)) {
        pipe_inc_ref(actual_old);
    }
    f->fd_map[newfd] = actual_old;
    return newfd;
}

int fcntl(int fd, int cmd, ...) {
    long arg = 0;
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC || cmd == F_SETFD || cmd == F_SETFL) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, long);
        va_end(ap);
    }
    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        init_fiber_subsystem();
        int actual = resolve_vfd(fd);
        if (actual < 0) {
            errno = EBADF;
            return -1;
        }
        int min_fd = (arg > 0) ? (int)arg : 0;
        if (is_pipe_fd(actual)) {
            pipe_inc_ref(actual);
            return actual;
        }
        if (actual == DEVNULL_FD) return DEVNULL_FD;
        ipo_fiber_t *f = &s_fibers[s_current_fiber];
        int start = (min_fd > 3) ? min_fd : 3;
        for (int v = start; v < f->fd_map_cap; v++) {
            if (f->fd_map && f->fd_map[v] < 0) {
                f->fd_map[v] = actual;
                return v;
            }
        }
        int new_v = (f->fd_map_cap > start) ? f->fd_map_cap : start;
        if (fiber_ensure_vfd(f, new_v) == 0) {
            f->fd_map[new_v] = actual;
            return new_v;
        }
        errno = EMFILE;
        return -1;
    }
    case F_GETFD:
    case F_SETFD:
    case F_GETFL:
    case F_SETFL:
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    init_fiber_subsystem();
    if (!fds && nfds > 0) {
        errno = EFAULT;
        return -1;
    }
    if (!fds || nfds == 0) {
        if (timeout > 0) {
            struct timespec req = { timeout / 1000, (timeout % 1000) * 1000000L };
            nanosleep(&req, NULL);
        }
        return 0;
    }

    struct timespec start_ts, now_ts;
    if (timeout > 0) {
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
    }

    for (;;) {
        int ready = 0;
        int has_blocking_pipe = 0;

        for (nfds_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            int actual = resolve_vfd(fds[i].fd);
            if (actual < 0) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }
            if (is_pipe_fd(actual)) {
                int idx = pipe_index_of(actual);
                ipo_pipe_t *p = &s_pipes[idx];
                if (!pipe_is_writer(actual)) {
                    if (p->head < p->len) {
                        if (fds[i].events & POLLIN) fds[i].revents |= POLLIN;
                    } else if (p->writer_refs <= 0) {
                        fds[i].revents |= POLLHUP;
                        if (fds[i].events & POLLIN) fds[i].revents |= POLLIN;
                    } else {
                        has_blocking_pipe = 1;
                    }
                } else {
                    if (p->reader_refs <= 0) {
                        fds[i].revents |= POLLERR | POLLHUP;
                    } else if (fds[i].events & POLLOUT) {
                        fds[i].revents |= POLLOUT;
                    }
                }
            } else {
                fds[i].revents = (short)(fds[i].events & (POLLIN | POLLOUT));
            }
            if (fds[i].revents) ready++;
        }

        if (ready > 0 || timeout == 0) {
            return ready;
        }

        if (has_blocking_pipe) {
            for (nfds_t i = 0; i < nfds; i++) {
                if (fds[i].fd < 0) continue;
                int actual = resolve_vfd(fds[i].fd);
                if (is_pipe_fd(actual) && !pipe_is_writer(actual)) {
                    int idx = pipe_index_of(actual);
                    ipo_pipe_t *p = &s_pipes[idx];
                    if (p->head >= p->len && p->writer_refs > 0 &&
                        p->writer_fiber_id >= 0 && !s_fibers[p->writer_fiber_id].finished) {
                        int next = pick_runnable_fiber_for_pipe(idx);
                        if (next >= 0 && next != s_current_fiber) {
                            s_fibers[s_current_fiber].waiting_on_pipe = idx;
                            fiber_switch_to(next);
                            s_fibers[s_current_fiber].waiting_on_pipe = -1;
                        }
                    }
                }
            }
        }

        if (timeout > 0) {
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            int64_t elapsed_ms = ((int64_t)now_ts.tv_sec - (int64_t)start_ts.tv_sec) * 1000LL +
                                 ((int64_t)now_ts.tv_nsec - (int64_t)start_ts.tv_nsec) / 1000000LL;
            if (elapsed_ms >= (int64_t)timeout) {
                return 0;
            }
        } else if (!has_blocking_pipe) {
            return 0;
        }

        struct timespec req = { 0, 1000000L };
        nanosleep(&req, NULL);
    }
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
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
        int64_t ms = (int64_t)timeout->tv_sec * 1000LL + (timeout->tv_usec + 999LL) / 1000LL;
        timeout_ms = (ms > 0x7FFFFFFFLL) ? 0x7FFFFFFF : (int)ms;
    }

    size_t cap = (nfds > 0) ? (size_t)nfds : 1u;
    struct pollfd *pfds = (struct pollfd *)calloc(cap, sizeof(struct pollfd));
    if (!pfds) {
        errno = ENOMEM;
        return -1;
    }

    nfds_t count = 0u;
    for (int fd = 0; fd < nfds; fd++) {
        short ev = 0;
        if (readfds && FD_ISSET(fd, readfds)) ev |= POLLIN;
        if (writefds && FD_ISSET(fd, writefds)) ev |= POLLOUT;
        if (ev || (exceptfds && FD_ISSET(fd, exceptfds))) {
            pfds[count].fd = fd;
            pfds[count].events = ev;
            count++;
        }
    }

    int pr = poll(pfds, count, timeout_ms);
    if (pr < 0) {
        int saved = errno;
        free(pfds);
        errno = saved;
        return -1;
    }

    if (readfds) FD_ZERO(readfds);
    if (writefds) FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);

    int ready_bits = 0;
    for (nfds_t i = 0; i < count; i++) {
        int fd = pfds[i].fd;
        short rev = pfds[i].revents;
        if (rev & POLLNVAL) {
            free(pfds);
            errno = EBADF;
            return -1;
        }
        if (readfds && (rev & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(fd, readfds);
            ready_bits++;
        }
        if (writefds && (rev & (POLLOUT | POLLERR | POLLHUP))) {
            FD_SET(fd, writefds);
            ready_bits++;
        }
        if (exceptfds && (rev & POLLERR)) {
            FD_SET(fd, exceptfds);
            ready_bits++;
        }
    }

    free(pfds);
    return ready_bits;
}

extern int __real_open(const char *pathname, int flags, ...);

int __wrap_open(const char *pathname, int flags, ...) {
    mode_t mode = 0666;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (pathname && strcmp(pathname, "/dev/null") == 0) {
        return DEVNULL_FD;
    }
    return __real_open(pathname, flags, mode);
}

/* --------------------------------------------------------------------------
 * Fiber Lifecycle & Trampoline
 * -------------------------------------------------------------------------- */
static void finish_current_fiber(int code) __attribute__((noreturn));

static void finish_current_fiber(int code) {
    ipo_fiber_t *f = &s_fibers[s_current_fiber];
    fflush(stdout);
    fflush(stderr);
    f->exit_code = code;
    f->finished = 1;
    f->waiting_on_pipe = -1;

    if (f->owns_stdio_fds && f->fd_map) {
        for (int v = 0; v < 3 && v < f->fd_map_cap; v++) {
            if (f->fd_map[v] >= 0) {
                int actual = f->fd_map[v];
                f->fd_map[v] = -1;
                raw_close_actual_fd(actual);
            }
        }
    }

    /* Clear writer_fiber_id / reader_fiber_id references for this fiber */
    for (int i = 0; i < s_pipes_cap; i++) {
        if (s_pipes[i].in_use) {
            if (s_pipes[i].writer_fiber_id == s_current_fiber)
                s_pipes[i].writer_fiber_id = -1;
            if (s_pipes[i].reader_fiber_id == s_current_fiber)
                s_pipes[i].reader_fiber_id = -1;
        }
    }

    /* Switch to runnable fiber */
    int target = -1;
    if (f->parent_fiber_id >= 0 && f->parent_fiber_id < s_fibers_cap &&
        s_fibers[f->parent_fiber_id].in_use && !s_fibers[f->parent_fiber_id].finished) {
        int wp = s_fibers[f->parent_fiber_id].waiting_on_pipe;
        if (wp < 0 || (wp < s_pipes_cap && (s_pipes[wp].head < s_pipes[wp].len || s_pipes[wp].writer_refs <= 0))) {
            target = f->parent_fiber_id;
        } else {
            target = pick_runnable_fiber_for_pipe(wp);
        }
    }
    if (target < 0 || target == s_current_fiber) {
        for (int i = s_fibers_cap - 1; i >= 0; i--) {
            if (i != s_current_fiber && s_fibers[i].in_use && !s_fibers[i].finished) {
                target = i;
                break;
            }
        }
    }
    if (target < 0) target = 0;
    fiber_switch_to(target);
    __builtin_unreachable();
}

static void ipo_fiber_trampoline(void) {
    ipo_fiber_t *f = &s_fibers[s_current_fiber];
    f->started = 1;
    int rc = 1;

    if (f->kind == 1) {
        rc = cmd_remote_curl(f->argc, (const char **)f->argv);
    } else if (f->kind == 2) {
        rc = cmd_fetch_pack(f->argc, (const char **)f->argv,
                            the_repository ? the_repository->prefix : NULL,
                            the_repository);
    } else if (f->kind == 3) {
        rc = cmd_send_pack(f->argc, (const char **)f->argv,
                           the_repository ? the_repository->prefix : NULL,
                           the_repository);
    } else if (f->kind == 4 && f->async_fn) {
        rc = f->async_fn(f->async_in, f->async_out, f->async_data);
    }

    finish_current_fiber(rc);
}

static int allocate_fiber(int kind, int is_async) {
    init_fiber_subsystem();
    int slot = -1;
    for (int i = 1; i < s_fibers_cap; i++) {
        if (!s_fibers[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        int old_cap = s_fibers_cap > 0 ? s_fibers_cap : 1;
        if (ensure_fibers_cap(old_cap + 1) < 0) return -1;
        slot = old_cap;
    }
    unsigned char *stk = (unsigned char *)malloc(FIBER_STACK_SZ);
    if (!stk) return -1;
    ipo_fiber_t *f = &s_fibers[slot];
    free(f->fd_map);
    memset(f, 0, sizeof(*f));
    f->id = slot;
    f->in_use = 1;
    f->started = 0;
    f->finished = 0;
    f->is_async = is_async;
    f->exit_code = 0;
    f->parent_fiber_id = s_current_fiber;
    f->waiting_on_pipe = -1;
    f->stack_base = stk;
    f->kind = kind;
    (void)fiber_ensure_vfd(f, 2);
    f->saved_stdin.fd = 0;
    f->saved_stdout.fd = 1;

    /* Prepare initial stack frame for ipo_fiber_switch */
    uint32_t top = ((uint32_t)(uintptr_t)(stk + FIBER_STACK_SZ)) & ~0xFu;
    uint32_t *sp = (uint32_t *)(uintptr_t)top;
    *(--sp) = 0; /* dummy return addr */
    *(--sp) = (uint32_t)(uintptr_t)ipo_fiber_trampoline; /* ret target */
    *(--sp) = 0; /* ebp */
    *(--sp) = 0; /* ebx */
    *(--sp) = 0; /* esi */
    *(--sp) = 0; /* edi */
    f->esp = (uint32_t)(uintptr_t)sp;
    return slot;
}

static void release_fiber(int fid) {
    if (fid <= 0 || fid >= s_fibers_cap) return;
    ipo_fiber_t *f = &s_fibers[fid];
    if (!f->in_use) return;
    if (f->argv) {
        for (int i = 0; i < f->argc; i++) {
            free(f->argv[i]);
        }
        free(f->argv);
        f->argv = NULL;
    }
    free(f->fd_map);
    f->fd_map = NULL;
    f->fd_map_cap = 0;
    free(f->stack_base);
    f->stack_base = NULL;
    f->in_use = 0;
    f->started = 0;
    f->finished = 0;
}

/* --------------------------------------------------------------------------
 * Child Process Table (Fibers + Real OS Processes via ipo_exec)
 * -------------------------------------------------------------------------- */
#define MAKE_CHILD_PID(cidx) ((pid_t)(10000 + (cidx) * 2))
#define MAKE_FIBER_PID(fid)  ((pid_t)(10001 + (fid) * 2))
#define IS_FIBER_PID(pid)    (((pid) >= 10001) && (((pid) - 10001) & 1) == 0)
#define FIBER_ID_FROM_PID(pid) (((int)(pid) - 10001) / 2)

typedef struct {
    int in_use;
    pid_t pid;
    int is_fiber;
    int fiber_id;
    int pending;
    int exit_code;

    /* For pending / OS child execution */
    int argc;
    char **argv;
    int envc;
    char **envv;
    char *dir;
    int in_pipe_idx;
    int out_pipe_wfd;
    int caller_out_fd;
    int no_stdout;
    int stdout_to_stderr;
} ipo_child_slot_t;

static ipo_child_slot_t *s_children = NULL;
static int s_children_cap = 0;
static int s_spawn_seq = 0;

static int ensure_children_cap(int min_cap) {
    if (min_cap <= s_children_cap) return 0;
    int ncap = s_children_cap > 0 ? s_children_cap : 32;
    while (ncap < min_cap) {
        if (ncap > INT32_MAX / 2) { ncap = min_cap; break; }
        ncap *= 2;
    }
    if ((size_t)ncap > SIZE_MAX / sizeof(ipo_child_slot_t)) return -1;
    ipo_child_slot_t *nc = (ipo_child_slot_t *)realloc(s_children, (size_t)ncap * sizeof(ipo_child_slot_t));
    if (!nc) return -1;
    memset(&nc[s_children_cap], 0, (size_t)(ncap - s_children_cap) * sizeof(ipo_child_slot_t));
    s_children = nc;
    s_children_cap = ncap;
    return 0;
}

static char *ipo_alloc_fmt(const char *fmt, ...) {
    va_list ap, aq;
    va_start(ap, fmt);
    va_copy(aq, ap);
    int n = vsnprintf(NULL, 0, fmt, aq);
    va_end(aq);
    if (n < 0) {
        va_end(ap);
        return NULL;
    }
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) {
        va_end(ap);
        return NULL;
    }
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

static char *ipo_getcwd_alloc(void) {
    size_t cap = 256;
    for (;;) {
        char *buf = (char *)malloc(cap);
        if (!buf) return NULL;
        if (getcwd(buf, cap)) return buf;
        free(buf);
        if (errno != ERANGE || cap > SIZE_MAX / 2) return NULL;
        cap *= 2;
    }
}

static void free_child_slot_strings(ipo_child_slot_t *c) {
    if (c->argv) {
        for (int i = 0; i < c->argc; i++) free(c->argv[i]);
        free(c->argv);
        c->argv = NULL;
    }
    if (c->envv) {
        for (int i = 0; i < c->envc; i++) free(c->envv[i]);
        free(c->envv);
        c->envv = NULL;
    }
    free(c->dir);
    c->dir = NULL;
}

static int write_buf_to_temp_file(const char *path, const void *data, size_t len) {
    int fd = __real_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    const unsigned char *p = (const unsigned char *)data;
    size_t rem = len;
    while (rem > 0) {
        ssize_t w = __real_write(fd, p, rem);
        if (w <= 0) {
            __real_close(fd);
            return -1;
        }
        p += (size_t)w;
        rem -= (size_t)w;
    }
    __real_close(fd);
    return 0;
}

static unsigned char *read_temp_file_all(const char *path, size_t *out_len) {
    *out_len = 0;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return NULL;
    }
    int fd = __real_open(path, O_RDONLY, 0);
    if (fd < 0) return NULL;
    size_t sz = (size_t)st.st_size;
    unsigned char *buf = (unsigned char *)malloc(sz + 1);
    if (!buf) {
        __real_close(fd);
        return NULL;
    }
    size_t total = 0;
    while (total < sz) {
        ssize_t r = __real_read(fd, buf + total, sz - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    __real_close(fd);
    buf[total] = '\0';
    *out_len = total;
    return buf;
}

static int ipo_exec_safe(const char *path, int argc, char **argv) {
    int pid = (int)getpid();
    if (pid < 1 || pid > 14) pid = 1;
    /*
     * Per-PID 64 KiB region at 0x0F600000 + pid * 0x10000.
     * This range (< 0x10000000, outside 0x00800000 binary_base) remains
     * identity-mapped across parent, child, and kernel page directories
     * during synchronous child execution inside int $0x80 (SYS_PROC_EXEC).
     */
    uint8_t *region = (uint8_t *)(uintptr_t)(0x0F600000u + (uint32_t)pid * 0x10000u);
    char **sh_argv = (char **)(uintptr_t)region;
    char *str_cur = (char *)(region + ((size_t)(argc + 2) * sizeof(char *)));
    char *str_end = (char *)(region + 0x8000u);

    char *sh_path = str_cur;
    size_t plen = strlen(path) + 1;
    if (str_cur + plen >= str_end) return -1;
    memcpy(sh_path, path, plen);
    str_cur += plen;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i] ? argv[i] : "";
        size_t alen = strlen(a) + 1;
        if (str_cur + alen >= str_end) return -1;
        sh_argv[i] = str_cur;
        memcpy(str_cur, a, alen);
        str_cur += alen;
    }
    sh_argv[argc] = NULL;

    uint32_t new_sp = ((uint32_t)(uintptr_t)(region + 0x10000u) - 64u) & ~15u;
    uint32_t *sys_args = (uint32_t *)(uintptr_t)new_sp;
    sys_args[0] = (uint32_t)(uintptr_t)sh_path;
    sys_args[1] = (uint32_t)argc;
    sys_args[2] = (uint32_t)(uintptr_t)sh_argv;

    int ret;
    __asm__ volatile(
        "movl %%esp, %%edx\n\t"
        "movl %%ebp, %%esi\n\t"
        "movl %%ecx, %%esp\n\t"
        "movl %%ecx, %%ebp\n\t"
        "pushl %%edx\n\t"
        "pushl %%esi\n\t"
        "int $0x80\n\t"
        "popl %%esi\n\t"
        "popl %%edx\n\t"
        "movl %%esi, %%ebp\n\t"
        "movl %%edx, %%esp\n\t"
        : "=a"(ret)
        : "a"((uint32_t)IPO_SYSCALL_EXEC), "b"(3u), "c"(new_sp)
        : "edx", "esi", "memory", "cc"
    );
    return ret;
}

static int handle_inproc_credential_store(ipo_child_slot_t *c, int start_idx) {
    const char *cred_file = "/.git-credentials";
    const char *action = "get";
    for (int i = start_idx + 1; i < c->argc; i++) {
        if (!c->argv[i]) continue;
        if (strcmp(c->argv[i], "--file") == 0 && i + 1 < c->argc) {
            cred_file = c->argv[++i];
        } else if (strncmp(c->argv[i], "--file=", 7) == 0) {
            cred_file = c->argv[i] + 7;
        } else if (c->argv[i][0] != '-') {
            action = c->argv[i];
        }
    }

    if (strcmp(action, "get") == 0) {
        size_t cred_len = 0;
        unsigned char *cred_buf = read_temp_file_all(cred_file, &cred_len);
        if (cred_buf && cred_len > 0) {
            char *line = (char *)cred_buf;
            while (*line == '\r' || *line == '\n' || *line == ' ') line++;
            char *eol = strpbrk(line, "\r\n");
            if (eol) *eol = '\0';
            const char *scheme_end = strstr(line, "://");
            const char *at = scheme_end ? strrchr(scheme_end + 3, '@') : NULL;
            if (scheme_end && at) {
                const char *userinfo = scheme_end + 3;
                const char *colon = memchr(userinfo, ':', (size_t)(at - userinfo));
                if (colon) {
                    size_t ulen = (size_t)(colon - userinfo);
                    size_t plen = (size_t)(at - (colon + 1));
                    struct strbuf ob = STRBUF_INIT;
                    strbuf_addstr(&ob, "username=");
                    strbuf_add(&ob, userinfo, ulen);
                    strbuf_addstr(&ob, "\npassword=");
                    strbuf_add(&ob, colon + 1, plen);
                    strbuf_addch(&ob, '\n');
                    if (ob.len > 0 && !c->no_stdout) {
                        if (c->out_pipe_wfd >= 0) {
                            __wrap_write(c->out_pipe_wfd, ob.buf, ob.len);
                        } else if (c->caller_out_fd >= 0) {
                            __wrap_write(c->caller_out_fd, ob.buf, ob.len);
                        }
                    }
                    strbuf_release(&ob);
                }
            }
        }
        free(cred_buf);
    }

    if (c->out_pipe_wfd >= 0) {
        raw_close_actual_fd(c->out_pipe_wfd);
        c->out_pipe_wfd = -1;
    }
    if (c->caller_out_fd >= 0) {
        raw_close_actual_fd(c->caller_out_fd);
        c->caller_out_fd = -1;
    }
    c->exit_code = 0;
    c->pending = 0;
    return 0;
}

static int execute_os_child_now(ipo_child_slot_t *c, const void *stdin_data, size_t stdin_len, int has_stdin) {
    int start_idx_check = (c->argc > 0 && c->argv[0] &&
                           (strcmp(c->argv[0], "git") == 0 ||
                            strcmp(c->argv[0], "/applications/git") == 0)) ? 1 : 0;
    if (start_idx_check < c->argc && c->argv[start_idx_check] &&
        strcmp(c->argv[start_idx_check], "credential-store") == 0) {
        return handle_inproc_credential_store(c, start_idx_check);
    }

    int seq = ++s_spawn_seq;
    int my_pid = (int)getpid();

    char *cfg_path = ipo_alloc_fmt("/tmp/.git_cfg_%d_%d", my_pid, seq);
    char *in_path = ipo_alloc_fmt("/tmp/.git_in_%d_%d", my_pid, seq);
    char *out_path = ipo_alloc_fmt("/tmp/.git_out_%d_%d", my_pid, seq);
    if (!cfg_path || !in_path || !out_path) {
        free(cfg_path);
        free(in_path);
        free(out_path);
        return -1;
    }

    mkdir("/tmp", 0755);

    if (has_stdin) {
        write_buf_to_temp_file(in_path, stdin_data ? stdin_data : "", stdin_len);
    }

    int capture_out = (c->out_pipe_wfd >= 0 || c->caller_out_fd >= 0 || c->no_stdout);

    /* Build config file for child */
    struct strbuf cfg = STRBUF_INIT;
    if (c->dir && c->dir[0]) {
        char *abs_dir = realpath(c->dir, NULL);
        strbuf_addf(&cfg, "CWD=%s\n", abs_dir ? abs_dir : c->dir);
        free(abs_dir);
    } else {
        char *cwd_buf = ipo_getcwd_alloc();
        strbuf_addf(&cfg, "CWD=%s\n", cwd_buf ? cwd_buf : "/");
        free(cwd_buf);
    }

    strbuf_addf(&cfg, "STDIN=%s\n", has_stdin ? in_path : "");
    strbuf_addf(&cfg, "STDOUT=%s\n", capture_out ? out_path : "");

    /* Export current environment */
    if (environ) {
        for (char **ep = environ; *ep; ep++) {
            strbuf_addf(&cfg, "ENV:%s\n", *ep);
        }
    }
    /* Apply cmd->env overrides */
    for (int i = 0; i < c->envc; i++) {
        if (!c->envv[i]) continue;
        if (strchr(c->envv[i], '=')) {
            strbuf_addf(&cfg, "ENV:%s\n", c->envv[i]);
        } else {
            strbuf_addf(&cfg, "UNSET:%s\n", c->envv[i]);
        }
    }

    write_buf_to_temp_file(cfg_path, cfg.buf, cfg.len);
    strbuf_release(&cfg);

    /* Determine executable path and arguments */
    const char *exec_path = "/applications/git";
    char *alloc_exec_path = NULL;
    int is_git_exec = 1;
    if (c->argc > 0 && c->argv[0]) {
        if (strcmp(c->argv[0], "git") != 0 && strncmp(c->argv[0], "/applications/git", 17) != 0) {
            char *cand = (c->argv[0][0] == '/')
                ? strdup(c->argv[0])
                : ipo_alloc_fmt("/applications/%s", c->argv[0]);
            struct stat st;
            if (cand && stat(cand, &st) == 0 && !S_ISDIR(st.st_mode)) {
                /* Wait: only use cand if it's NOT a git subcommand */
                if (strcmp(c->argv[0], "edit") == 0 || strcmp(c->argv[0], "cat") == 0) {
                    alloc_exec_path = cand;
                    exec_path = alloc_exec_path;
                    cand = NULL;
                    is_git_exec = 0;
                }
            }
            free(cand);
        }
    }

    char *spawn_arg = ipo_alloc_fmt("--ipo-spawn=%s", cfg_path);
    int exec_argc = 0;
    char **exec_argv = (char **)calloc((size_t)c->argc + 4, sizeof(char *));
    if (!spawn_arg || !exec_argv) {
        free(alloc_exec_path);
        free(spawn_arg);
        free(exec_argv);
        unlink(cfg_path);
        if (has_stdin) unlink(in_path);
        free(cfg_path);
        free(in_path);
        free(out_path);
        return -1;
    }

    if (is_git_exec) {
        exec_argv[exec_argc++] = (char *)"/applications/git";
        exec_argv[exec_argc++] = spawn_arg;
        int start_idx = (c->argc > 0 && (strcmp(c->argv[0], "git") == 0 ||
                         strcmp(c->argv[0], "/applications/git") == 0)) ? 1 : 0;
        for (int i = start_idx; i < c->argc; i++) {
            exec_argv[exec_argc++] = c->argv[i];
        }
    } else {
        for (int i = 0; i < c->argc; i++) {
            exec_argv[exec_argc++] = c->argv[i];
        }
    }
    exec_argv[exec_argc] = NULL;

    char *saved_cwd = ipo_getcwd_alloc();

    int child_pid = ipo_exec_safe(exec_path, exec_argc, exec_argv);
    int exit_code = (child_pid < 0) ? 127 : ipo_get_exit_code();

    /* Restore parent's working directory in case child changed global kernel cwd */
    ipo_chdir(saved_cwd ? saved_cwd : "/");
    free(saved_cwd);
    free(alloc_exec_path);
    free(spawn_arg);
    free(exec_argv);
    unlink(cfg_path);
    if (has_stdin) unlink(in_path);

    if (capture_out) {
        size_t out_len = 0;
        unsigned char *out_data = read_temp_file_all(out_path, &out_len);
        unlink(out_path);
        if (!c->no_stdout && out_data && out_len > 0) {
            if (c->out_pipe_wfd >= 0) {
                __wrap_write(c->out_pipe_wfd, out_data, out_len);
            } else if (c->caller_out_fd >= 0) {
                __wrap_write(c->caller_out_fd, out_data, out_len);
            }
        }
        free(out_data);
        if (c->out_pipe_wfd >= 0) {
            raw_close_actual_fd(c->out_pipe_wfd);
            c->out_pipe_wfd = -1;
        }
        if (c->caller_out_fd >= 0) {
            raw_close_actual_fd(c->caller_out_fd);
            c->caller_out_fd = -1;
        }
    }
    free(cfg_path);
    free(in_path);
    free(out_path);

    c->exit_code = exit_code;
    c->pending = 0;
    return exit_code;
}

static void run_pending_child_for_pipe(int pipe_idx) {
    if (pipe_idx < 0 || pipe_idx >= s_pipes_cap) return;
    ipo_pipe_t *p = &s_pipes[pipe_idx];
    int cidx = p->pending_child_idx;
    if (cidx < 0 || cidx >= s_children_cap) return;
    p->pending_child_idx = -1;

    ipo_child_slot_t *c = &s_children[cidx];
    if (!c->in_use || !c->pending) return;

    const void *in_ptr = p->buf ? (const void *)(p->buf + p->head) : (const void *)"";
    size_t in_len = (p->len >= p->head) ? (p->len - p->head) : 0;
    p->head = p->len = 0;
    if (p->reader_refs > 0) p->reader_refs--;

    execute_os_child_now(c, in_ptr, in_len, 1);
}

/* --------------------------------------------------------------------------
 * Shell Command Splitter & Command Classifier
 * -------------------------------------------------------------------------- */
static void split_shell_command_into_strvec(const char *cmd_str, struct strvec *out) {
    if (!cmd_str) return;
    const char *p = cmd_str;
    struct strbuf tok = STRBUF_INIT;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        strbuf_reset(&tok);
        char quote = 0;
        while (*p) {
            char c = *p;
            if (quote) {
                if (c == quote) {
                    quote = 0;
                    p++;
                    continue;
                }
                if (c == '\\' && quote == '"' && p[1]) {
                    p++;
                    c = *p;
                }
                strbuf_addch(&tok, c);
                p++;
            } else {
                if (c == '\'' || c == '"') {
                    quote = c;
                    p++;
                    continue;
                }
                if (c == '\\' && p[1]) {
                    p++;
                    c = *p;
                    strbuf_addch(&tok, c);
                    p++;
                    continue;
                }
                if (c == ' ' || c == '\t' || c == '\n') break;
                strbuf_addch(&tok, c);
                p++;
            }
        }
        strvec_push(out, tok.buf);
    }
    strbuf_release(&tok);
}

static int classify_fiber_command(int argc, char **argv) {
    if (argc <= 0 || !argv || !argv[0]) return 0;
    const char *cmd = argv[0];
    const char *slash = strrchr(cmd, '/');
    if (slash) cmd = slash + 1;
    if (strncmp(cmd, "git-", 4) == 0) cmd += 4;
    else if (strcmp(cmd, "git") == 0 && argc > 1 && argv[1]) {
        cmd = argv[1];
        if (strncmp(cmd, "git-", 4) == 0) cmd += 4;
    }

    if (strcmp(cmd, "remote-http") == 0 ||
        strcmp(cmd, "remote-https") == 0 ||
        strcmp(cmd, "remote-ftp") == 0 ||
        strcmp(cmd, "remote-ftps") == 0 ||
        strcmp(cmd, "remote-curl") == 0) {
        return 1; /* cmd_remote_curl */
    }
    if (strcmp(cmd, "fetch-pack") == 0) {
        for (int i = 1; i < argc; i++) {
            if (argv[i] && strcmp(argv[i], "--stateless-rpc") == 0)
                return 2; /* cmd_fetch_pack */
        }
    }
    if (strcmp(cmd, "send-pack") == 0) {
        for (int i = 1; i < argc; i++) {
            if (argv[i] && strcmp(argv[i], "--stateless-rpc") == 0)
                return 3; /* cmd_send_pack */
        }
    }
    return 0;
}

static int is_noop_background_cmd(int argc, char **argv) {
    if (argc <= 0 || !argv || !argv[0]) return 1;
    const char *cmd = argv[0];
    const char *slash = strrchr(cmd, '/');
    if (slash) cmd = slash + 1;
    if (strcmp(cmd, "git") == 0 && argc > 1) cmd = argv[1];
    if (strcmp(cmd, "maintenance") == 0 || strcmp(cmd, "gc") == 0) {
        for (int i = 1; i < argc; i++) {
            if (argv[i] && strcmp(argv[i], "--auto") == 0)
                return 1;
        }
    }
    if (strcmp(cmd, "infocmp") == 0 || strcmp(cmd, "stty") == 0) {
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * start_command / finish_command / start_async / finish_async
 * -------------------------------------------------------------------------- */
int start_command(struct child_process *cmd) {
    init_fiber_subsystem();
    if (!cmd || cmd->args.nr == 0 || !cmd->args.v[0]) {
        errno = EINVAL;
        return -1;
    }

    struct strvec norm_args = STRVEC_INIT;
    if (cmd->use_shell && cmd->args.nr == 1) {
        split_shell_command_into_strvec(cmd->args.v[0], &norm_args);
    } else {
        for (size_t i = 0; i < cmd->args.nr; i++) {
            strvec_push(&norm_args, cmd->args.v[i]);
        }
    }

    if (norm_args.nr == 0 || !norm_args.v[0]) {
        strvec_clear(&norm_args);
        child_process_clear(cmd);
        errno = ENOENT;
        return -1;
    }

    int cidx = -1;
    for (int i = 0; i < s_children_cap; i++) {
        if (!s_children[i].in_use) {
            cidx = i;
            break;
        }
    }
    if (cidx < 0) {
        int old_cap = s_children_cap;
        if (ensure_children_cap(old_cap + 1) < 0) {
            strvec_clear(&norm_args);
            child_process_clear(cmd);
            errno = EAGAIN;
            return -1;
        }
        cidx = old_cap;
    }

    ipo_child_slot_t *c = &s_children[cidx];
    memset(c, 0, sizeof(*c));
    c->in_use = 1;
    c->in_pipe_idx = -1;
    c->out_pipe_wfd = -1;
    c->caller_out_fd = -1;

    /* Fast path: skip background auto-maintenance / infocmp / stty */
    if (is_noop_background_cmd((int)norm_args.nr, (char **)norm_args.v)) {
        if (cmd->in > 0) __wrap_close(cmd->in);
        else if (cmd->in == -1) {
            int p[2];
            if (pipe(p) == 0) {
                raw_close_actual_fd(p[0]);
                cmd->in = p[1];
            }
        }
        if (cmd->out > 0) __wrap_close(cmd->out);
        else if (cmd->out == -1) {
            int p[2];
            if (pipe(p) == 0) {
                raw_close_actual_fd(p[1]);
                cmd->out = p[0];
            }
        }
        if (cmd->err > 0) __wrap_close(cmd->err);
        else if (cmd->err == -1) {
            int p[2];
            if (pipe(p) == 0) {
                raw_close_actual_fd(p[1]);
                cmd->err = p[0];
            }
        }
        c->pid = MAKE_CHILD_PID(cidx);
        c->exit_code = 0;
        cmd->pid = c->pid;
        strvec_clear(&norm_args);
        return 0;
    }

    int fkind = classify_fiber_command((int)norm_args.nr, (char **)norm_args.v);
    if (fkind > 0) {
        int fid = allocate_fiber(fkind, 0);
        if (fid < 0) {
            c->in_use = 0;
            strvec_clear(&norm_args);
            child_process_clear(cmd);
            errno = ENOMEM;
            return -1;
        }
        ipo_fiber_t *f = &s_fibers[fid];
        f->owns_stdio_fds = 1;

        size_t start_i = 0;
        if (norm_args.nr > 1 && strcmp(norm_args.v[0], "git") == 0) {
            start_i = 1;
        }
        f->argc = (int)(norm_args.nr - start_i);
        f->argv = (char **)calloc((size_t)f->argc + 1, sizeof(char *));
        for (int i = 0; i < f->argc; i++) {
            f->argv[i] = strdup(norm_args.v[start_i + (size_t)i]);
        }
        f->argv[f->argc] = NULL;

        if (cmd->no_stdin) {
            f->fd_map[0] = DEVNULL_FD;
        } else if (cmd->in == -1) {
            int p[2];
            if (pipe(p) < 0) {
                release_fiber(fid);
                c->in_use = 0;
                strvec_clear(&norm_args);
                child_process_clear(cmd);
                return -1;
            }
            f->fd_map[0] = p[0];
            cmd->in = p[1];
            s_pipes[pipe_index_of(p[0])].reader_fiber_id = fid;
            s_pipes[pipe_index_of(p[1])].writer_fiber_id = s_current_fiber;
        } else if (cmd->in > 0) {
            int actual = resolve_vfd(cmd->in);
            ipo_fiber_t *curf = &s_fibers[s_current_fiber];
            if (cmd->in < curf->fd_map_cap && curf->fd_map && curf->fd_map[cmd->in] >= 0)
                curf->fd_map[cmd->in] = -1;
            f->fd_map[0] = actual;
            if (is_pipe_fd(actual))
                s_pipes[pipe_index_of(actual)].reader_fiber_id = fid;
        }

        if (cmd->no_stdout) {
            f->fd_map[1] = DEVNULL_FD;
        } else if (cmd->stdout_to_stderr) {
            f->fd_map[1] = 2;
        } else if (cmd->out == -1) {
            int p[2];
            if (pipe(p) < 0) {
                release_fiber(fid);
                c->in_use = 0;
                strvec_clear(&norm_args);
                child_process_clear(cmd);
                return -1;
            }
            f->fd_map[1] = p[1];
            cmd->out = p[0];
            s_pipes[pipe_index_of(p[1])].writer_fiber_id = fid;
            s_pipes[pipe_index_of(p[0])].reader_fiber_id = s_current_fiber;
        } else if (cmd->out > 0) {
            int actual = resolve_vfd(cmd->out);
            ipo_fiber_t *curf = &s_fibers[s_current_fiber];
            if (cmd->out < curf->fd_map_cap && curf->fd_map && curf->fd_map[cmd->out] >= 0)
                curf->fd_map[cmd->out] = -1;
            f->fd_map[1] = actual;
            if (is_pipe_fd(actual))
                s_pipes[pipe_index_of(actual)].writer_fiber_id = fid;
        }

        if (cmd->no_stderr) {
            f->fd_map[2] = DEVNULL_FD;
        } else if (cmd->err == -1) {
            int p[2];
            if (pipe(p) == 0) {
                f->fd_map[2] = p[1];
                cmd->err = p[0];
                s_pipes[pipe_index_of(p[1])].writer_fiber_id = fid;
            }
        } else if (cmd->err > 0) {
            int actual = resolve_vfd(cmd->err);
            ipo_fiber_t *curf = &s_fibers[s_current_fiber];
            if (cmd->err < curf->fd_map_cap && curf->fd_map && curf->fd_map[cmd->err] >= 0)
                curf->fd_map[cmd->err] = -1;
            f->fd_map[2] = actual;
        }

        c->is_fiber = 1;
        c->fiber_id = fid;
        c->pid = MAKE_FIBER_PID(fid);
        cmd->pid = c->pid;
        strvec_clear(&norm_args);
        return 0;
    }

    /* OS Child Process path (via ipo_exec) */
    c->is_fiber = 0;
    c->pid = MAKE_CHILD_PID(cidx);
    cmd->pid = c->pid;
    c->no_stdout = cmd->no_stdout;
    c->stdout_to_stderr = cmd->stdout_to_stderr;
    c->dir = cmd->dir ? strdup(cmd->dir) : NULL;

    c->argc = (int)norm_args.nr;
    c->argv = (char **)calloc((size_t)c->argc + 1, sizeof(char *));
    for (int i = 0; i < c->argc; i++) {
        c->argv[i] = strdup(norm_args.v[i]);
    }
    c->argv[c->argc] = NULL;
    strvec_clear(&norm_args);

    c->envc = (int)cmd->env.nr;
    if (c->envc > 0) {
        c->envv = (char **)calloc((size_t)c->envc + 1, sizeof(char *));
        for (int i = 0; i < c->envc; i++) {
            c->envv[i] = cmd->env.v[i] ? strdup(cmd->env.v[i]) : NULL;
        }
    }

    if (cmd->out == -1) {
        int out_p[2];
        if (pipe(out_p) < 0) {
            free_child_slot_strings(c);
            c->in_use = 0;
            child_process_clear(cmd);
            return -1;
        }
        cmd->out = out_p[0];
        c->out_pipe_wfd = out_p[1];
    } else if (cmd->out > 0) {
        int actual = resolve_vfd(cmd->out);
        ipo_fiber_t *curf = &s_fibers[s_current_fiber];
        if (cmd->out < curf->fd_map_cap && curf->fd_map && curf->fd_map[cmd->out] >= 0)
            curf->fd_map[cmd->out] = -1;
        c->caller_out_fd = actual;
    }

    if (cmd->err == -1) {
        int err_p[2];
        if (pipe(err_p) == 0) {
            raw_close_actual_fd(err_p[1]);
            cmd->err = err_p[0];
        }
    } else if (cmd->err > 0) {
        __wrap_close(cmd->err);
    }

    if (cmd->in == -1) {
        int in_p[2];
        if (pipe(in_p) < 0) {
            if (c->out_pipe_wfd >= 0) raw_close_actual_fd(c->out_pipe_wfd);
            free_child_slot_strings(c);
            c->in_use = 0;
            child_process_clear(cmd);
            return -1;
        }
        int pidx = pipe_index_of(in_p[0]);
        s_pipes[pidx].pending_child_idx = cidx;
        c->in_pipe_idx = pidx;
        c->pending = 1;
        cmd->in = in_p[1];
        return 0;
    } else if (cmd->in > 0) {
        drain_pipe_from_writer_fiber(cmd->in);
        struct strbuf in_data = STRBUF_INIT;
        unsigned char tmp[16384];
        for (;;) {
            ssize_t r = __wrap_read(cmd->in, tmp, sizeof(tmp));
            if (r <= 0) break;
            strbuf_add(&in_data, tmp, (size_t)r);
        }
        __wrap_close(cmd->in);
        execute_os_child_now(c, in_data.buf, in_data.len, 1);
        strbuf_release(&in_data);
        return 0;
    } else {
        execute_os_child_now(c, "", 0, cmd->no_stdin ? 1 : 0);
        return 0;
    }
}

int finish_command(struct child_process *cmd) {
    init_fiber_subsystem();
    if (!cmd) return -1;
    int cidx = -1;
    for (int i = 0; i < s_children_cap; i++) {
        if (s_children[i].in_use && s_children[i].pid == cmd->pid) {
            cidx = i;
            break;
        }
    }
    if (cidx < 0) {
        child_process_clear(cmd);
        return 0;
    }
    ipo_child_slot_t *c = &s_children[cidx];
    int rc = 0;

    if (c->is_fiber) {
        int fid = c->fiber_id;
        while (fid > 0 && fid < s_fibers_cap && s_fibers[fid].in_use && !s_fibers[fid].finished) {
            int wp = s_fibers[fid].waiting_on_pipe;
            if (wp >= 0) {
                int next = pick_runnable_fiber_for_pipe(wp);
                if (next >= 0 && next != s_current_fiber) {
                    fiber_switch_to(next);
                    continue;
                }
            }
            fiber_switch_to(fid);
        }
        rc = (fid > 0 && fid < s_fibers_cap) ? s_fibers[fid].exit_code : 0;
        release_fiber(fid);
    } else {
        if (c->pending && c->in_pipe_idx >= 0) {
            run_pending_child_for_pipe(c->in_pipe_idx);
        }
        rc = c->exit_code;
    }

    free_child_slot_strings(c);
    memset(c, 0, sizeof(*c));
    child_process_clear(cmd);
    return rc;
}

int finish_command_in_signal(struct child_process *cmd) {
    return finish_command(cmd);
}

int run_command(struct child_process *cmd) {
    int code;
    if (!cmd) return -1;
    if (cmd->out < 0 || cmd->err < 0) {
        errno = EINVAL;
        return -1;
    }
    code = start_command(cmd);
    if (code)
        return code;
    return finish_command(cmd);
}

int pipe_command(struct child_process *cmd,
                 const char *in, size_t in_len,
                 struct strbuf *out, size_t out_hint,
                 struct strbuf *err, size_t err_hint) {
    if (!cmd) return -1;
    if (in) cmd->in = -1;
    if (out) cmd->out = -1;
    if (err) cmd->err = -1;
    if (start_command(cmd) < 0)
        return -1;

    if (in && cmd->in >= 0) {
        const char *p = in;
        size_t rem = in_len;
        while (rem > 0) {
            ssize_t w = __wrap_write(cmd->in, p, rem);
            if (w <= 0) break;
            p += (size_t)w;
            rem -= (size_t)w;
        }
        __wrap_close(cmd->in);
    }
    if (out && cmd->out >= 0) {
        if (out_hint) strbuf_grow(out, out_hint);
        char buf[4096];
        for (;;) {
            ssize_t r = __wrap_read(cmd->out, buf, sizeof(buf));
            if (r <= 0) break;
            strbuf_add(out, buf, (size_t)r);
        }
        __wrap_close(cmd->out);
    }
    if (err && cmd->err >= 0) {
        if (err_hint) strbuf_grow(err, err_hint);
        char buf[4096];
        for (;;) {
            ssize_t r = __wrap_read(cmd->err, buf, sizeof(buf));
            if (r <= 0) break;
            strbuf_add(err, buf, (size_t)r);
        }
        __wrap_close(cmd->err);
    }
    return finish_command(cmd);
}

void run_processes_parallel(const struct run_process_parallel_opts *opts) {
    if (!opts || !opts->get_next_task) return;
    for (;;) {
        struct child_process cmd = CHILD_PROCESS_INIT;
        struct strbuf err_buf = STRBUF_INIT;
        void *task_cb = NULL;
        if (!opts->get_next_task(&cmd, &err_buf, opts->data, &task_cb)) {
            strbuf_release(&err_buf);
            break;
        }
        int code = start_command(&cmd);
        if (code < 0) {
            if (opts->start_failure) {
                opts->start_failure(&err_buf, opts->data, task_cb);
            }
            if (err_buf.len) {
                __wrap_write(2, err_buf.buf, err_buf.len);
            }
            strbuf_release(&err_buf);
            continue;
        }
        if (opts->feed_pipe && cmd.in >= 0) {
            while (opts->feed_pipe(cmd.in, opts->data, task_cb) == 0) {
            }
            __wrap_close(cmd.in);
            cmd.in = -1;
        }
        int res = finish_command(&cmd);
        if (err_buf.len) {
            __wrap_write(2, err_buf.buf, err_buf.len);
        }
        if (opts->task_finished) {
            opts->task_finished(res, &err_buf, opts->data, task_cb);
        }
        strbuf_release(&err_buf);
    }
}

enum start_bg_result start_bg_command(struct child_process *cmd,
                                      start_bg_wait_cb *wait_cb,
                                      void *cb_data,
                                      unsigned int timeout_sec) {
    (void)wait_cb;
    (void)cb_data;
    (void)timeout_sec;
    int rc = run_command(cmd);
    return (rc == 0) ? SBGR_READY : SBGR_ERROR;
}

int start_async(struct async *async) {
    init_fiber_subsystem();
    if (!async || !async->proc) return -1;

    int need_in = async->in < 0;
    int need_out = async->out < 0;
    int fdin[2] = { -1, -1 };
    int fdout[2] = { -1, -1 };
    int proc_in = -1, proc_out = -1;

    if (need_in) {
        if (pipe(fdin) < 0) return -1;
        async->in = fdin[1];
        proc_in = fdin[0];
    } else if (async->in > 0) {
        proc_in = async->in;
    }

    if (need_out) {
        if (pipe(fdout) < 0) {
            if (need_in) {
                __wrap_close(fdin[0]);
                __wrap_close(fdin[1]);
            }
            return -1;
        }
        async->out = fdout[0];
        proc_out = fdout[1];
    } else if (async->out > 0) {
        proc_out = async->out;
    }

    int fid = allocate_fiber(4, 1);
    if (fid < 0) return -1;

    ipo_fiber_t *f = &s_fibers[fid];
    ipo_fiber_t *par = &s_fibers[s_current_fiber];
    f->owns_stdio_fds = 0;
    /* Inherit parent fiber's fd_map so fd 0/1/2 inside sideband_demux resolve to parent's pipes */
    if (par->fd_map_cap > 0 && par->fd_map && fiber_ensure_vfd(f, par->fd_map_cap - 1) == 0) {
        for (int j = 0; j < par->fd_map_cap; j++) {
            f->fd_map[j] = par->fd_map[j];
        }
    }
    f->async_fn = async->proc;
    f->async_in = proc_in;
    f->async_out = proc_out;
    f->async_data = async->data;

    if (is_pipe_fd(proc_in))
        s_pipes[pipe_index_of(proc_in)].reader_fiber_id = fid;
    if (is_pipe_fd(proc_out))
        s_pipes[pipe_index_of(proc_out)].writer_fiber_id = fid;

    async->pid = MAKE_FIBER_PID(fid);
    return 0;
}

int finish_async(struct async *async) {
    init_fiber_subsystem();
    if (!async) return 0;
    if (!IS_FIBER_PID(async->pid)) return 0;
    int fid = FIBER_ID_FROM_PID(async->pid);
    if (fid <= 0 || fid >= s_fibers_cap || !s_fibers[fid].in_use) {
        return 0;
    }
    while (s_fibers[fid].in_use && !s_fibers[fid].finished) {
        int wp = s_fibers[fid].waiting_on_pipe;
        if (wp >= 0) {
            int next = pick_runnable_fiber_for_pipe(wp);
            if (next >= 0 && next != s_current_fiber) {
                fiber_switch_to(next);
                continue;
            }
        }
        fiber_switch_to(fid);
    }
    int rc = s_fibers[fid].exit_code;
    release_fiber(fid);
    return rc;
}

int in_async(void) {
    return (s_current_fiber > 0 && s_fibers[s_current_fiber].is_async) ? 1 : 0;
}

int async_with_fork(void) {
    return 0;
}

int prepare_auto_maintenance(struct repository *r, int quiet, struct child_process *maint) {
    (void)r;
    (void)quiet;
    (void)maint;
    return 0;
}

/* --------------------------------------------------------------------------
 * remote-curl.c integration helpers
 * -------------------------------------------------------------------------- */
const char *ipo_remote_curl_setup_git_dir(struct repository *repo, int *nongit_ok) {
    if (repo && repo->gitdir && repo->objects) {
        if (nongit_ok) *nongit_ok = 0;
        if (startup_info) startup_info->have_repository = 1;
        return NULL;
    }
    return setup_git_directory_gently(repo, nongit_ok);
}

void ipo_remote_curl_trace2_cmd_name(const char *name) {
    (void)name;
}

/* --------------------------------------------------------------------------
 * Process Entry (__wrap_main) & Exit (__wrap_exit / __wrap__exit)
 * -------------------------------------------------------------------------- */
static char *s_initial_cwd = NULL;
static char *s_child_stdout_file = NULL;
static int s_child_stdout_rfd = -1;

void __wrap_exit(int status) {
    if (s_current_fiber != 0) {
        finish_current_fiber(status);
    }
    fflush(stdout);
    fflush(stderr);

    if (s_child_stdout_file && s_child_stdout_rfd >= 0) {
        if (s_fibers && s_fibers[0].fd_map && s_fibers[0].fd_map_cap > 1 && s_fibers[0].fd_map[1] >= 0) {
            int wfd = s_fibers[0].fd_map[1];
            s_fibers[0].fd_map[1] = -1;
            raw_close_actual_fd(wfd);
        }
        int pidx = pipe_index_of(s_child_stdout_rfd);
        ipo_pipe_t *p = &s_pipes[pidx];
        const void *ptr = (p->in_use && p->buf) ? (const void *)(p->buf + p->head) : (const void *)"";
        size_t len = (p->in_use && p->len >= p->head) ? (p->len - p->head) : 0;
        write_buf_to_temp_file(s_child_stdout_file, ptr, len);
        raw_close_actual_fd(s_child_stdout_rfd);
        s_child_stdout_rfd = -1;
        free(s_child_stdout_file);
        s_child_stdout_file = NULL;
    }

    if (s_initial_cwd && s_initial_cwd[0]) {
        ipo_chdir(s_initial_cwd);
        free(s_initial_cwd);
        s_initial_cwd = NULL;
    }
    __real_exit(status);
}

void __wrap__exit(int status) {
    __wrap_exit(status);
}

static void apply_spawn_config_file(const char *cfg_path) {
    size_t len = 0;
    unsigned char *raw = read_temp_file_all(cfg_path, &len);
    if (!raw) return;

    char *p = (char *)raw;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';

        if (strncmp(p, "CWD=", 4) == 0) {
            const char *dir = p + 4;
            if (dir[0]) chdir(dir);
        } else if (strncmp(p, "STDIN=", 6) == 0) {
            const char *in_file = p + 6;
            if (in_file[0]) {
                int pfd[2];
                if (pipe(pfd) == 0) {
                    size_t in_len = 0;
                    unsigned char *in_data = read_temp_file_all(in_file, &in_len);
                    if (in_data && in_len > 0) {
                        __wrap_write(pfd[1], in_data, in_len);
                    }
                    free(in_data);
                    raw_close_actual_fd(pfd[1]);
                    if (fiber_ensure_vfd(&s_fibers[0], 0) == 0) {
                        s_fibers[0].fd_map[0] = pfd[0];
                    }
                }
            }
        } else if (strncmp(p, "STDOUT=", 7) == 0) {
            const char *out_file = p + 7;
            if (out_file[0]) {
                int pfd[2];
                if (pipe(pfd) == 0) {
                    s_child_stdout_rfd = pfd[0];
                    if (fiber_ensure_vfd(&s_fibers[0], 1) == 0) {
                        s_fibers[0].fd_map[1] = pfd[1];
                    }
                    s_child_stdout_file = strdup(out_file);
                }
            }
        } else if (strncmp(p, "ENV:", 4) == 0) {
            char *kv = p + 4;
            char *eq = strchr(kv, '=');
            if (eq) {
                *eq = '\0';
                setenv(kv, eq + 1, 1);
            }
        } else if (strncmp(p, "UNSET:", 6) == 0) {
            unsetenv(p + 6);
        }

        if (!eol) break;
        p = eol + 1;
    }
    free(raw);
}

static void ensure_default_gitconfig(void) {
    struct stat st;
    if (stat("/.gitconfig", &st) != 0) {
        const char *def_cfg =
            "[user]\n"
            "\tname = IPOleksenko\n"
            "\temail = 1ipooip1@gmail.com\n"
            "[init]\n"
            "\tdefaultBranch = main\n"
            "[core]\n"
            "\tfilemode = false\n"
            "\tpager = cat\n"
            "[credential]\n"
            "\thelper = store\n"
            "[http]\n"
            "\tsslVerify = false\n"
            "[maintenance]\n"
            "\tauto = false\n"
            "[gc]\n"
            "\tauto = 0\n";
        write_buf_to_temp_file("/.gitconfig", def_cfg, strlen(def_cfg));
    }
}

int __wrap_main(int argc, const char **argv) {
    init_fiber_subsystem();
    s_initial_cwd = ipo_getcwd_alloc();
    if (!s_initial_cwd) s_initial_cwd = strdup("/");

    mkdir("/tmp", 0755);

    setenv("HOME", "/", 0);
    setenv("PATH", "/applications", 0);
    setenv("GIT_EXEC_PATH", "/applications", 0);
    setenv("GIT_PAGER", "cat", 0);
    setenv("PAGER", "cat", 0);
    setenv("GIT_EDITOR", "/applications/edit", 0);
    setenv("EDITOR", "/applications/edit", 0);
    setenv("TERM", "dumb", 0);
    setenv("GIT_SSL_NO_VERIFY", "1", 0);
    setenv("GIT_TERMINAL_PROMPT", "1", 0);
    setenv("GIT_TEST_MAINT_AUTO_DETACH", "0", 0);

    ensure_default_gitconfig();

    if (argc >= 2 && argv[1] && strncmp(argv[1], "--ipo-spawn=", 12) == 0) {
        const char *cfg_path = argv[1] + 12;
        apply_spawn_config_file(cfg_path);
        argv[1] = argv[0];
        argv++;
        argc--;
    }

    int rc = __real_main(argc, argv);
    __wrap_exit(rc);
}

/* --------------------------------------------------------------------------
 * POSIX Compatibility Shims for IPO_OS
 * -------------------------------------------------------------------------- */
mode_t umask(mode_t mask) {
    static mode_t s_umask = 022;
    mode_t old = s_umask;
    s_umask = mask & 0777;
    return old;
}

int chmod(const char *pathname, uint32_t mode) {
    (void)pathname;
    (void)mode;
    return 0;
}

int fchmod(int fd, mode_t mode) {
    (void)fd;
    (void)mode;
    return 0;
}

int chown(const char *pathname, uid_t owner, gid_t group) {
    (void)pathname;
    (void)owner;
    (void)group;
    return 0;
}

int lchown(const char *pathname, uid_t owner, gid_t group) {
    (void)pathname;
    (void)owner;
    (void)group;
    return 0;
}

int link(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    errno = EPERM;
    return -1;
}

int symlink(const char *target, const char *linkpath) {
    (void)target;
    (void)linkpath;
    errno = EPERM;
    return -1;
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    (void)pathname;
    (void)buf;
    (void)bufsiz;
    errno = EINVAL;
    return -1;
}

int utimes(const char *filename, const struct timeval times[2]) {
    (void)filename;
    (void)times;
    return 0;
}

uid_t geteuid(void) { return 0; }
gid_t getegid(void) { return 0; }
pid_t getpgid(pid_t pid) { return pid > 0 ? pid : getpid(); }
pid_t getpgrp(void) { return getpid(); }
int setpgid(pid_t pid, pid_t pgid) { (void)pid; (void)pgid; return 0; }
pid_t setsid(void) { return getpid(); }

static struct passwd s_root_pw = {
    .pw_name = (char *)"IPOleksenko",
    .pw_passwd = (char *)"x",
    .pw_uid = 0,
    .pw_gid = 0,
    .pw_gecos = (char *)"IPOleksenko",
    .pw_dir = (char *)"/",
    .pw_shell = (char *)"/applications/sh"
};

struct passwd *getpwuid(uid_t uid) {
    (void)uid;
    return &s_root_pw;
}

struct passwd *getpwnam(const char *name) {
    (void)name;
    return &s_root_pw;
}

static char *s_grp_members[] = { (char *)"IPOleksenko", NULL };
static struct group s_root_gr = {
    .gr_name = (char *)"root",
    .gr_passwd = (char *)"x",
    .gr_gid = 0,
    .gr_mem = s_grp_members
};

struct group *getgrgid(gid_t gid) {
    (void)gid;
    return &s_root_gr;
}

struct group *getgrnam(const char *name) {
    (void)name;
    return &s_root_gr;
}

int gethostname(char *name, size_t len) {
    if (!name || len == 0) {
        errno = EINVAL;
        return -1;
    }
    strncpy(name, "ipo-os", len - 1);
    name[len - 1] = '\0';
    return 0;
}

int uname(struct utsname *buf) {
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    strcpy(buf->sysname, "IPO_OS");
    strcpy(buf->nodename, "ipo-os");
    strcpy(buf->release, "1.0");
    strcpy(buf->version, "IPO_OS 1.0");
    strcpy(buf->machine, "i686");
    return 0;
}

unsigned int alarm(unsigned int seconds) {
    (void)seconds;
    return 0;
}

void tzset(void) {}

char *ctermid(char *s) {
    static char buf[16] = "/dev/tty";
    if (s) {
        strcpy(s, "/dev/tty");
        return s;
    }
    return buf;
}

pid_t tcgetpgrp(int fd) {
    (void)fd;
    return getpid();
}

int tcsetpgrp(int fd, pid_t pgrp) {
    (void)fd;
    (void)pgrp;
    return 0;
}

int tcgetattr(int fd, struct termios *termios_p) {
    (void)fd;
    if (termios_p) memset(termios_p, 0, sizeof(*termios_p));
    return 0;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    (void)fd;
    (void)optional_actions;
    (void)termios_p;
    return 0;
}

int killpg(int pgrp, int sig) {
    return kill(pgrp, sig);
}

int statvfs(const char *path, struct statvfs *buf) {
    if (!path || !buf) {
        errno = !path ? ENOENT : EFAULT;
        return -1;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    uint32_t kbuf[16];
    memset(kbuf, 0, sizeof(kbuf));
    uint32_t args[1] = { (uint32_t)(uintptr_t)kbuf };
    if (ipo_syscall(IPO_SYSCALL_FS_STATVFS, 1u, args) != 0) {
        errno = EIO;
        return -1;
    }
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize   = kbuf[0] ? kbuf[0] : 4096u;
    buf->f_frsize  = kbuf[1] ? kbuf[1] : buf->f_bsize;
    buf->f_blocks  = kbuf[2];
    buf->f_bfree   = kbuf[3];
    buf->f_bavail  = kbuf[4];
    buf->f_files   = kbuf[5];
    buf->f_ffree   = kbuf[6];
    buf->f_favail  = kbuf[6];
    buf->f_namemax = 255u;
    return 0;
}

char *getpass(const char *prompt) {
    static char *s_pass_buf = NULL;
    static size_t s_pass_cap = 0;
    if (prompt && *prompt) {
        __real_write(2, prompt, strlen(prompt));
    }
    char *line = NULL;
    int n = ipo_read_line_dynamic(&line);
    if (n < 0 || !line) {
        return NULL;
    }
    size_t need = (size_t)n + 1;
    if (need > s_pass_cap) {
        char *nb = (char *)realloc(s_pass_buf, need);
        if (!nb) {
            ipo_kfree(line);
            return NULL;
        }
        s_pass_buf = nb;
        s_pass_cap = need;
    }
    if (n > 0) memcpy(s_pass_buf, line, (size_t)n);
    s_pass_buf[n] = '\0';
    ipo_kfree(line);
    return s_pass_buf;
}

pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

int execl(const char *path, const char *arg, ...) {
    (void)path;
    (void)arg;
    errno = ENOSYS;
    return -1;
}

int execlp(const char *file, const char *arg, ...) {
    (void)file;
    (void)arg;
    errno = ENOSYS;
    return -1;
}

int shutdown(int sockfd, int how) {
    (void)sockfd;
    (void)how;
    return 0;
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen) {
    (void)sockfd;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return 0;
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    (void)sockfd;
    (void)level;
    (void)optname;
    if (optval && optlen && *optlen >= sizeof(int)) {
        *(int *)optval = 0;
    }
    return 0;
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    (void)sockfd;
    if (addr && addrlen) memset(addr, 0, *addrlen);
    return 0;
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    (void)sockfd;
    if (addr && addrlen) memset(addr, 0, *addrlen);
    return 0;
}

int h_errno = 0;

const char *hstrerror(int err) {
    (void)err;
    return "Resolver error";
}

typedef struct {
    struct hostent he;
    struct in_addr addr;
    char *addr_list[2];
    char *aliases[1];
    char *name_buf;
    size_t name_cap;
    uint32_t tid;
} hostent_tls_t;

static volatile unsigned char s_he_lock = 0;
static hostent_tls_t **s_he_slots = NULL;
static size_t s_he_count = 0u;
static size_t s_he_cap = 0u;

static hostent_tls_t *acquire_hostent_tls(hostent_tls_t *fallback) {
    uint32_t tid = (uint32_t)ipo_syscall(IPO_SYSCALL_THREAD_SELF, 0u, NULL);
    hostent_tls_t *found = NULL;

    while (__atomic_test_and_set(&s_he_lock, __ATOMIC_ACQUIRE)) {
        ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
    }

    for (size_t i = 0u; i < s_he_count; ++i) {
        if (s_he_slots[i] && s_he_slots[i]->tid == tid) {
            found = s_he_slots[i];
            break;
        }
    }

    if (!found) {
        hostent_tls_t *t = (hostent_tls_t *)calloc(1u, sizeof(hostent_tls_t));
        if (t) {
            t->tid = tid;
            if (s_he_count == s_he_cap) {
                size_t new_cap = s_he_cap ? (s_he_cap * 2u) : 8u;
                hostent_tls_t **new_slots = (hostent_tls_t **)realloc(
                    s_he_slots, new_cap * sizeof(hostent_tls_t *));
                if (new_slots) {
                    s_he_slots = new_slots;
                    s_he_cap = new_cap;
                }
            }
            if (s_he_count < s_he_cap) {
                s_he_slots[s_he_count++] = t;
                found = t;
            } else {
                free(t);
            }
        }
    }

    __atomic_clear(&s_he_lock, __ATOMIC_RELEASE);
    return found ? found : fallback;
}

struct hostent *gethostbyname(const char *name) {
    static hostent_tls_t s_fallback_he;
    if (!name || !*name) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    uint32_t ip_host = 0u;
    uint32_t args[3] = {
        (uint32_t)(uintptr_t)name,
        (uint32_t)(uintptr_t)&ip_host,
        3000u
    };
    if (ipo_syscall(IPO_SYSCALL_NET_DNS_RESOLVE, 3u, args) != 1 || ip_host == 0u) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }

    hostent_tls_t *st = acquire_hostent_tls(&s_fallback_he);

    size_t nlen = strlen(name);
    if (nlen + 1u > st->name_cap) {
        char *nb = (char *)realloc(st->name_buf, nlen + 1u);
        if (!nb) {
            h_errno = NO_RECOVERY;
            return NULL;
        }
        st->name_buf = nb;
        st->name_cap = nlen + 1u;
    }
    memcpy(st->name_buf, name, nlen + 1u);
    st->addr.s_addr = htonl(ip_host);
    st->addr_list[0] = (char *)&st->addr;
    st->addr_list[1] = NULL;
    st->aliases[0] = NULL;
    st->he.h_name = st->name_buf;
    st->he.h_aliases = st->aliases;
    st->he.h_addrtype = AF_INET;
    st->he.h_length = 4;
    st->he.h_addr_list = st->addr_list;
    h_errno = 0;
    return &st->he;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (af != AF_INET || !src || !dst || size < 16) {
        errno = ENOSPC;
        return NULL;
    }
    const uint8_t *b = (const uint8_t *)src;
    snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return dst;
}

int inet_pton(int af, const char *src, void *dst) {
    if (af != AF_INET || !src || !dst) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    unsigned int a, b, c, d;
    if (sscanf(src, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    uint8_t *out = (uint8_t *)dst;
    out[0] = (uint8_t)a;
    out[1] = (uint8_t)b;
    out[2] = (uint8_t)c;
    out[3] = (uint8_t)d;
    return 1;
}

char *inet_ntoa(struct in_addr in) {
    static char buf[16];
    inet_ntop(AF_INET, &in, buf, sizeof(buf));
    return buf;
}

int execve(const char *pathname, char *const argv[], char *const envp[]) {
    (void)envp;
    return execv(pathname, argv);
}

#include <sys/ioctl.h>

struct servent *getservbyname(const char *name, const char *proto) {
    static struct servent s_se;
    static char *s_aliases[1] = { NULL };
    static char *s_name = NULL;
    static size_t s_name_cap = 0;
    static char *s_proto = NULL;
    static size_t s_proto_cap = 0;
    int port = 9418;
    if (name) {
        if (strcmp(name, "http") == 0) port = 80;
        else if (strcmp(name, "https") == 0) port = 443;
        else if (strcmp(name, "ssh") == 0) port = 22;
        else if (strcmp(name, "git") == 0) port = 9418;
        else {
            int p = atoi(name);
            if (p > 0) port = p;
        }
    }
    const char *n_src = name ? name : "git";
    const char *p_src = proto ? proto : "tcp";
    size_t nlen = strlen(n_src) + 1u;
    size_t plen = strlen(p_src) + 1u;
    if (nlen > s_name_cap) {
        char *nn = (char *)realloc(s_name, nlen);
        if (!nn) return NULL;
        s_name = nn;
        s_name_cap = nlen;
    }
    if (plen > s_proto_cap) {
        char *np = (char *)realloc(s_proto, plen);
        if (!np) return NULL;
        s_proto = np;
        s_proto_cap = plen;
    }
    memcpy(s_name, n_src, nlen);
    memcpy(s_proto, p_src, plen);
    s_se.s_name = s_name;
    s_se.s_aliases = s_aliases;
    s_se.s_port = htons((uint16_t)port);
    s_se.s_proto = s_proto;
    return &s_se;
}

int ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    if (request == TIOCGWINSZ) {
        va_list ap;
        va_start(ap, request);
        struct winsize *ws = va_arg(ap, struct winsize *);
        va_end(ap);
        if (ws) {
            ws->ws_row = 25;
            ws->ws_col = 80;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}
