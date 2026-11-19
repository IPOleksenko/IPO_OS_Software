#include "links.h"

#include <errno.h>
#include <fcntl.h>
#include <net/socket.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <syscall.h>
#include <vga.h>
#include <unistd.h>
#include <driver/input/keyboard.h>

extern bool keyboard_is_ctrl_pressed(void);
extern const char *keyboard_get_key_string(uint8_t scancode);

#define IPO_SOCKET_FD_BASE 128
#define IPO_PIPE_FD_BASE   64
#define MAX_PIPES          8
#define PIPE_BUF_SIZE      4096

typedef struct {
    int flags;
    int error;
    int handle;
    unsigned char in_use;
    unsigned char connected;
} ipo_fd_slot_t;

static ipo_fd_slot_t *s_fd_slots = NULL;
static int s_fd_slots_cap = 0;

static int ensure_fd_slot(int fd) {
    if (fd < 0) return -1;
    if (fd < s_fd_slots_cap) return 0;
    int new_cap = (s_fd_slots_cap > 0) ? s_fd_slots_cap : (IPO_SOCKET_FD_BASE * 2);
    while (new_cap <= fd) {
        new_cap *= 2;
    }
    ipo_fd_slot_t *n = (ipo_fd_slot_t *)realloc(s_fd_slots, (size_t)new_cap * sizeof(ipo_fd_slot_t));
    if (!n) return -1;
    memset(n + s_fd_slots_cap, 0, (size_t)(new_cap - s_fd_slots_cap) * sizeof(ipo_fd_slot_t));
    s_fd_slots = n;
    s_fd_slots_cap = new_cap;
    return 0;
}

/* Pipe in-memory emulation */
typedef struct {
    unsigned char buffer[PIPE_BUF_SIZE];
    size_t head;
    size_t tail;
    size_t count;
    int reader_open;
    int writer_open;
    int in_use;
} inmem_pipe_t;

static inmem_pipe_t s_pipes[MAX_PIPES];

static int is_pipe_fd(int fd) {
    return fd >= IPO_PIPE_FD_BASE && fd < (IPO_PIPE_FD_BASE + MAX_PIPES * 2);
}

int ipo_pipe_has_data(int fd) {
    if (!is_pipe_fd(fd)) return 0;
    int pipe_idx = (fd - IPO_PIPE_FD_BASE) / 2;
    int is_reader = ((fd - IPO_PIPE_FD_BASE) % 2) == 0;
    if (!is_reader || !s_pipes[pipe_idx].in_use) return 0;
    return s_pipes[pipe_idx].count > 0;
}

int ipo_pipe_has_space(int fd) {
    if (!is_pipe_fd(fd)) return 0;
    int pipe_idx = (fd - IPO_PIPE_FD_BASE) / 2;
    int is_writer = ((fd - IPO_PIPE_FD_BASE) % 2) == 1;
    if (!is_writer || !s_pipes[pipe_idx].in_use) return 0;
    return s_pipes[pipe_idx].count < PIPE_BUF_SIZE;
}

static ssize_t pipe_read(int fd, void *buf, size_t count) {
    int pipe_idx = (fd - IPO_PIPE_FD_BASE) / 2;
    int is_reader = ((fd - IPO_PIPE_FD_BASE) % 2) == 0;
    if (!is_reader || !s_pipes[pipe_idx].in_use) {
        errno = EBADF;
        return -1;
    }
    inmem_pipe_t *p = &s_pipes[pipe_idx];
    if (p->count == 0) {
        if (!p->writer_open) return 0; /* EOF */
        if (fd < s_fd_slots_cap && (s_fd_slots[fd].flags & O_NONBLOCK)) {
            errno = EAGAIN;
            return -1;
        }
        return 0;
    }
    size_t to_read = count < p->count ? count : p->count;
    unsigned char *dst = (unsigned char *)buf;
    for (size_t i = 0; i < to_read; i++) {
        dst[i] = p->buffer[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->count -= to_read;
    return (ssize_t)to_read;
}

static ssize_t pipe_write(int fd, const void *buf, size_t count) {
    int pipe_idx = (fd - IPO_PIPE_FD_BASE) / 2;
    int is_writer = ((fd - IPO_PIPE_FD_BASE) % 2) == 1;
    if (!is_writer || !s_pipes[pipe_idx].in_use || !s_pipes[pipe_idx].reader_open) {
        errno = EPIPE;
        return -1;
    }
    inmem_pipe_t *p = &s_pipes[pipe_idx];
    size_t space = PIPE_BUF_SIZE - p->count;
    if (space == 0) {
        if (fd < s_fd_slots_cap && (s_fd_slots[fd].flags & O_NONBLOCK)) {
            errno = EAGAIN;
            return -1;
        }
        return 0;
    }
    size_t to_write = count < space ? count : space;
    const unsigned char *src = (const unsigned char *)buf;
    for (size_t i = 0; i < to_write; i++) {
        p->buffer[p->head] = src[i];
        p->head = (p->head + 1) % PIPE_BUF_SIZE;
    }
    p->count += to_write;
    return (ssize_t)to_write;
}

static int pipe_close(int fd) {
    int pipe_idx = (fd - IPO_PIPE_FD_BASE) / 2;
    int is_reader = ((fd - IPO_PIPE_FD_BASE) % 2) == 0;
    if (!s_pipes[pipe_idx].in_use) return -1;
    if (is_reader) s_pipes[pipe_idx].reader_open = 0;
    else s_pipes[pipe_idx].writer_open = 0;

    if (!s_pipes[pipe_idx].reader_open && !s_pipes[pipe_idx].writer_open) {
        memset(&s_pipes[pipe_idx], 0, sizeof(inmem_pipe_t));
    }
    if (fd >= 0 && fd < s_fd_slots_cap) s_fd_slots[fd].flags = 0;
    return 0;
}

int pipe(int pipefd[2]) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!s_pipes[i].in_use) {
            int rfd = IPO_PIPE_FD_BASE + i * 2;
            int wfd = IPO_PIPE_FD_BASE + i * 2 + 1;
            if (ensure_fd_slot(wfd) < 0) {
                errno = ENOMEM;
                return -1;
            }
            memset(&s_pipes[i], 0, sizeof(inmem_pipe_t));
            s_pipes[i].in_use = 1;
            s_pipes[i].reader_open = 1;
            s_pipes[i].writer_open = 1;
            pipefd[0] = rfd;
            pipefd[1] = wfd;
            s_fd_slots[rfd].flags = 0;
            s_fd_slots[wfd].flags = 0;
            return 0;
        }
    }
    errno = EMFILE;
    return -1;
}

static int socket_handle(int fd) {
    if (fd < IPO_SOCKET_FD_BASE || fd >= s_fd_slots_cap || !s_fd_slots[fd].in_use) {
        errno = EBADF;
        return -1;
    }
    return s_fd_slots[fd].handle;
}

int socket(int domain, int type, int protocol) {
    uint32_t args[3] = { (uint32_t)domain, (uint32_t)type, (uint32_t)protocol };
    int handle = ipo_syscall(IPO_SYSCALL_NET_SOCKET, 3u, args);

    if (handle < 0) {
        errno = EIO;
        return -1;
    }
    int fd = IPO_SOCKET_FD_BASE;
    while (fd < s_fd_slots_cap && s_fd_slots[fd].in_use) {
        fd++;
    }
    if (ensure_fd_slot(fd) == 0) {
        s_fd_slots[fd].handle = handle;
        s_fd_slots[fd].in_use = 1;
        s_fd_slots[fd].flags = 0;
        s_fd_slots[fd].error = 0;
        s_fd_slots[fd].connected = 0;
        return fd;
    }

    uint32_t close_args[1] = { (uint32_t)handle };
    ipo_syscall(IPO_SYSCALL_NET_CLOSE, 1u, close_args);
    errno = EMFILE;
    return -1;
}

int ipo_libc_socket_is_fd(int fd) {
    return (fd >= IPO_SOCKET_FD_BASE && fd < s_fd_slots_cap && s_fd_slots[fd].in_use) || is_pipe_fd(fd);
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    int value;

    if (fd < 0 || ensure_fd_slot(fd) < 0) {
        errno = EBADF;
        return -1;
    }
    if (cmd == F_GETFD) return 0;
    if (cmd == F_GETFL) return s_fd_slots[fd].flags;

    va_start(ap, cmd);
    value = va_arg(ap, int);
    va_end(ap);
    if (cmd == F_SETFD) return value == 0 || value == FD_CLOEXEC ? 0 : (errno = EINVAL, -1);
    if (cmd == F_SETFL) {
        s_fd_slots[fd].flags = value & O_NONBLOCK;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen) {
    if (!ipo_libc_socket_is_fd(sockfd) || sockfd >= s_fd_slots_cap || level != SOL_SOCKET ||
        optname != SO_ERROR || !optval || !optlen || *optlen < sizeof(int)) {
        errno = EOPNOTSUPP;
        return -1;
    }
    *(int *)optval = s_fd_slots[sockfd].error;
    s_fd_slots[sockfd].error = 0;
    *optlen = sizeof(int);
    return 0;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    uint32_t args[3];
    int result;
    int handle = socket_handle(sockfd);

    if (handle < 0 || !addr || addrlen < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }
    args[0] = (uint32_t)handle;
    args[1] = (uint32_t)(uintptr_t)addr;
    args[2] = addrlen;
    result = ipo_syscall(IPO_SYSCALL_NET_CONNECT, 3u, args);
    s_fd_slots[sockfd].connected = result == 0;
    s_fd_slots[sockfd].error = result == 0 ? 0 : EIO;
    if (result != 0) errno = EIO;
    return result;
}

int ipo_links_socket_connected(int sockfd) {
    return sockfd >= IPO_SOCKET_FD_BASE && sockfd < s_fd_slots_cap && s_fd_slots[sockfd].in_use && s_fd_slots[sockfd].connected;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    if (is_pipe_fd(sockfd)) {
        return pipe_write(sockfd, buf, len);
    }
    uint32_t args[4];
    int result;
    int handle = socket_handle(sockfd);

    if (handle < 0 || (len && !buf) || len > INT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (!len) return 0;
    if (s_fd_slots[sockfd].flags & O_NONBLOCK) flags |= MSG_DONTWAIT;
    args[0] = (uint32_t)handle;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = (uint32_t)len;
    args[3] = (uint32_t)flags;
    result = ipo_syscall(IPO_SYSCALL_NET_SEND, 4u, args);
    if (result < 0) errno = ((s_fd_slots[sockfd].flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) ? EAGAIN : EIO;
    return result;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    if (is_pipe_fd(sockfd)) {
        return pipe_read(sockfd, buf, len);
    }
    uint32_t args[4];
    int result;
    int handle = socket_handle(sockfd);

    if (handle < 0 || (len && !buf) || len > INT32_MAX) {
        errno = EINVAL;
        return -1;
    }
    if (!len) return 0;
    if (s_fd_slots[sockfd].flags & O_NONBLOCK) flags |= MSG_DONTWAIT;
    args[0] = (uint32_t)handle;
    args[1] = (uint32_t)(uintptr_t)buf;
    args[2] = (uint32_t)len;
    args[3] = (uint32_t)flags;
    result = ipo_syscall(IPO_SYSCALL_NET_RECV, 4u, args);
    if (result < 0) errno = ((s_fd_slots[sockfd].flags & O_NONBLOCK) || (flags & MSG_DONTWAIT)) ? EAGAIN : EIO;
    return result;
}

ssize_t ipo_libc_socket_read(int fd, void *buf, size_t count) {
    if (is_pipe_fd(fd)) return pipe_read(fd, buf, count);
    return recv(fd, buf, count, 0);
}

ssize_t ipo_libc_socket_write(int fd, const void *buf, size_t count) {
    if (is_pipe_fd(fd)) return pipe_write(fd, buf, count);
    return send(fd, buf, count, 0);
}

int ipo_libc_socket_close(int fd) {
    if (is_pipe_fd(fd)) return pipe_close(fd);
    uint32_t args[1];
    int handle = socket_handle(fd);
    int result;

    if (handle < 0) return -1;
    args[0] = (uint32_t)handle;
    result = ipo_syscall(IPO_SYSCALL_NET_CLOSE, 1u, args);
    if (result < 0) {
        errno = EIO;
        return -1;
    }
    s_fd_slots[fd].in_use = 0;
    s_fd_slots[fd].connected = 0;
    s_fd_slots[fd].error = 0;
    s_fd_slots[fd].flags = 0;
    return 0;
}

/* Stdin raw keyboard handling */
static char s_kbd_buf[64];
static size_t s_kbd_head = 0;
static size_t s_kbd_tail = 0;

static void kbd_buf_push(char c) {
    size_t next = (s_kbd_head + 1) % sizeof(s_kbd_buf);
    if (next != s_kbd_tail) {
        s_kbd_buf[s_kbd_head] = c;
        s_kbd_head = next;
    }
}

static void kbd_buf_push_str(const char *str) {
    while (*str) kbd_buf_push(*str++);
}

int ipo_stdin_has_data(void) {
    if (s_kbd_head != s_kbd_tail) return 1;
    keyboard_poll();
    uint8_t sc = keyboard_get_scancode();
    if (sc == 0) return 0;
    if (sc & 0x80) return 0; /* key release */

    /* Translate scancode */
    if (sc == 0x48) kbd_buf_push_str("\033[A"); /* Up */
    else if (sc == 0x50) kbd_buf_push_str("\033[B"); /* Down */
    else if (sc == 0x4D) kbd_buf_push_str("\033[C"); /* Right */
    else if (sc == 0x4B) kbd_buf_push_str("\033[D"); /* Left */
    else if (sc == 0x1C) kbd_buf_push('\r'); /* Enter */
    else if (sc == 0x0E) kbd_buf_push('\b'); /* Backspace */
    else if (sc == 0x0F) kbd_buf_push('\t'); /* Tab */
    else if (sc == 0x01) kbd_buf_push('\033'); /* Escape */
    else if (sc == 0x49) kbd_buf_push_str("\033[5~"); /* PgUp */
    else if (sc == 0x51) kbd_buf_push_str("\033[6~"); /* PgDn */
    else if (sc == 0x2E && keyboard_is_ctrl_pressed()) kbd_buf_push(0x03); /* Ctrl+C */
    else {
        const char *ks = keyboard_get_key_string(sc);
        if (ks && *ks) kbd_buf_push_str(ks);
    }
    return s_kbd_head != s_kbd_tail;
}

ssize_t ipo_libc_stdin_read(void *buf, size_t count) {
    if (!count || !buf) return 0;
    if (!ipo_stdin_has_data()) {
        errno = EAGAIN;
        return -1;
    }
    char *dst = (char *)buf;
    size_t read_bytes = 0;
    while (read_bytes < count && s_kbd_head != s_kbd_tail) {
        dst[read_bytes++] = s_kbd_buf[s_kbd_tail];
        s_kbd_tail = (s_kbd_tail + 1) % sizeof(s_kbd_buf);
    }
    return (ssize_t)read_bytes;
}

int getentropy(void *buffer, size_t length) {
    if (!buffer) {
        errno = EFAULT;
        return -1;
    }
    unsigned char *p = (unsigned char *)buffer;
    uint32_t tsc_lo = 0, tsc_hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
    uint32_t ms = (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
    static uint32_t s_entropy_seq = 0x9E3779B9u;
    s_entropy_seq += 0x6C078965u;
    uint64_t state = ((uint64_t)tsc_hi << 32) | (uint64_t)tsc_lo;
    state ^= ((uint64_t)ms << 17) ^ ((uint64_t)(uintptr_t)buffer << 7) ^ (uint64_t)s_entropy_seq;
    if (state == 0) state = 0x85EBCA6B4F3D2195ULL;
    for (size_t i = 0; i < length; i++) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        uint64_t mixed = state * 0x2545F4914F6CDD1DULL;
        p[i] = (unsigned char)((mixed >> ((i & 7u) * 8u)) & 0xFFu);
    }
    return 0;
}

ssize_t __read_chk(int fd, void *buf, size_t nbytes, size_t buflen) {
    (void)buflen;
    return read(fd, buf, nbytes);
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    (void)sockfd; (void)addr; (void)addrlen;
    errno = EOPNOTSUPP;
    return -1;
}

int dup(int oldfd) {
    (void)oldfd;
    errno = ENOSYS;
    return -1;
}

int dup2(int oldfd, int newfd) {
    (void)oldfd; (void)newfd;
    errno = ENOSYS;
    return -1;
}

pid_t fork(void) {
    errno = ENOSYS;
    return -1;
}

int execl(const char *path, const char *arg, ...) {
    (void)path; (void)arg;
    errno = ENOSYS;
    return -1;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    (void)path; (void)buf; (void)bufsiz;
    errno = ENOSYS;
    return -1;
}

int exe(unsigned char *path, int fg) {
    (void)path; (void)fg;
    errno = ENOSYS;
    return -1;
}

void get_terminal_size(int *x, int *y) {
    if (x) *x = (int)VGA_WIDTH;
    if (y) *y = (int)VGA_HEIGHT;
}

void handle_terminal_resize(void (*resize)(int, int), int *x, int *y) {
    (void)resize;
    if (x) *x = (int)VGA_WIDTH;
    if (y) *y = (int)VGA_HEIGHT;
}

void unhandle_terminal_resize(void) {}

/* Direct VGA ANSI Terminal Driver for Links TUI */
static uint16_t *s_saved_vga = NULL;
static size_t s_saved_vga_cells = 0;
static int s_vga_saved = 0;
static int s_cur_x = 0;
static int s_cur_y = 0;
static uint8_t s_fg = 7;
static uint8_t s_bg = 0;
static uint8_t s_bold = 0;
static uint8_t s_rev = 0;

static const uint8_t ansi_to_vga_col[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static uint8_t compute_vga_attr(void) {
    uint8_t fg = ansi_to_vga_col[s_fg & 7];
    uint8_t bg = ansi_to_vga_col[s_bg & 7];
    if (s_bold) fg |= 0x08;
    if (s_rev) {
        uint8_t tmp = fg & 0x07;
        fg = (bg & 0x07) | (fg & 0x08);
        bg = tmp;
    }
    return ((bg & 0x07) << 4) | (fg & 0x0F);
}

static void vga_scroll_up(void) {
    int vw = (int)VGA_WIDTH;
    int vh = (int)VGA_HEIGHT;
    volatile uint16_t *vga = VGA_MEMORY;
    if (!vga || vw <= 0 || vh <= 1) return;
    for (int y = 0; y < vh - 1; y++) {
        for (int x = 0; x < vw; x++) {
            vga[y * vw + x] = vga[(y + 1) * vw + x];
        }
    }
    uint16_t fill = ((uint16_t)compute_vga_attr() << 8) | ' ';
    for (int x = 0; x < vw; x++) {
        vga[(vh - 1) * vw + x] = fill;
    }
}

enum {
    ANSI_STATE_NORMAL = 0,
    ANSI_STATE_ESC,
    ANSI_STATE_CSI,
    ANSI_STATE_CHARSET
};

static int s_ansi_state = ANSI_STATE_NORMAL;
static int s_csi_params[16];
static int s_csi_count = 0;
static int s_csi_has_val = 0;

void ipo_vga_ansi_write(const unsigned char *p, int l) {
    int vw = (int)VGA_WIDTH;
    int vh = (int)VGA_HEIGHT;
    volatile uint16_t *vga = VGA_MEMORY;
    if (!vga || vw <= 0 || vh <= 0) return;
    size_t cells = (size_t)vw * (size_t)vh;
    if (!s_vga_saved) {
        s_saved_vga = (uint16_t *)malloc(cells * sizeof(uint16_t));
        if (s_saved_vga) {
            s_saved_vga_cells = cells;
            for (size_t i = 0; i < cells; i++) {
                s_saved_vga[i] = vga[i];
            }
        }
        s_vga_saved = 1;
    }

    for (int i = 0; i < l; i++) {
        unsigned char ch = p[i];
        switch (s_ansi_state) {
            case ANSI_STATE_NORMAL:
                if (ch == 0x1B) {
                    s_ansi_state = ANSI_STATE_ESC;
                } else if (ch == '\r') {
                    s_cur_x = 0;
                } else if (ch == '\n') {
                    s_cur_y++;
                    if (s_cur_y >= vh) {
                        vga_scroll_up();
                        s_cur_y = vh - 1;
                    }
                } else if (ch == '\b') {
                    if (s_cur_x > 0) s_cur_x--;
                } else if (ch == '\t') {
                    s_cur_x = (s_cur_x + 8) & ~7;
                    if (s_cur_x >= vw) {
                        s_cur_x = 0;
                        s_cur_y++;
                        if (s_cur_y >= vh) {
                            vga_scroll_up();
                            s_cur_y = vh - 1;
                        }
                    }
                } else if (ch >= 32) {
                    vga[s_cur_y * vw + s_cur_x] = ((uint16_t)compute_vga_attr() << 8) | ch;
                    s_cur_x++;
                    if (s_cur_x >= vw) {
                        s_cur_x = 0;
                        s_cur_y++;
                        if (s_cur_y >= vh) {
                            vga_scroll_up();
                            s_cur_y = vh - 1;
                        }
                    }
                }
                break;

            case ANSI_STATE_ESC:
                if (ch == '[') {
                    s_ansi_state = ANSI_STATE_CSI;
                    s_csi_count = 0;
                    s_csi_params[0] = 0;
                    s_csi_has_val = 0;
                } else if (ch == '(' || ch == ')') {
                    s_ansi_state = ANSI_STATE_CHARSET;
                } else if (ch == 'M') {
                    if (s_cur_y > 0) s_cur_y--;
                    s_ansi_state = ANSI_STATE_NORMAL;
                } else {
                    s_ansi_state = ANSI_STATE_NORMAL;
                }
                break;

            case ANSI_STATE_CHARSET:
                s_ansi_state = ANSI_STATE_NORMAL;
                break;

            case ANSI_STATE_CSI:
                if (ch >= '0' && ch <= '9') {
                    s_csi_params[s_csi_count] = s_csi_params[s_csi_count] * 10 + (ch - '0');
                    s_csi_has_val = 1;
                } else if (ch == ';') {
                    if (s_csi_count < 15) s_csi_count++;
                    s_csi_params[s_csi_count] = 0;
                } else if (ch == '?') {
                    /* private mode flag - ignore */
                } else if (ch == 'H' || ch == 'f') {
                    int row = (s_csi_has_val && s_csi_params[0] > 0) ? s_csi_params[0] - 1 : 0;
                    int col = (s_csi_count >= 1 && s_csi_params[1] > 0) ? s_csi_params[1] - 1 : 0;
                    if (row < 0) row = 0;
                    if (row >= vh) row = vh - 1;
                    if (col < 0) col = 0;
                    if (col >= vw) col = vw - 1;
                    s_cur_y = row;
                    s_cur_x = col;
                    s_ansi_state = ANSI_STATE_NORMAL;
                } else if (ch == 'J') {
                    int mode = s_csi_params[0];
                    uint16_t fill = ((uint16_t)compute_vga_attr() << 8) | ' ';
                    if (mode == 2) {
                        for (size_t k = 0; k < cells; k++) vga[k] = fill;
                        s_cur_x = 0;
                        s_cur_y = 0;
                    } else if (mode == 0) {
                        for (size_t k = (size_t)(s_cur_y * vw + s_cur_x); k < cells; k++) vga[k] = fill;
                    }
                    s_ansi_state = ANSI_STATE_NORMAL;
                } else if (ch == 'K') {
                    uint16_t fill = ((uint16_t)compute_vga_attr() << 8) | ' ';
                    for (int k = s_cur_x; k < vw; k++) vga[s_cur_y * vw + k] = fill;
                    s_ansi_state = ANSI_STATE_NORMAL;
                } else if (ch == 'm') {
                    int count = s_csi_has_val ? s_csi_count + 1 : 1;
                    for (int k = 0; k < count; k++) {
                        int p = s_csi_params[k];
                        if (p == 0) { s_fg = 7; s_bg = 0; s_bold = 0; s_rev = 0; }
                        else if (p == 1) s_bold = 1;
                        else if (p == 7) s_rev = 1;
                        else if (p == 27) s_rev = 0;
                        else if (p >= 30 && p <= 37) s_fg = (uint8_t)(p - 30);
                        else if (p == 39) s_fg = 7;
                        else if (p >= 40 && p <= 47) s_bg = (uint8_t)(p - 40);
                        else if (p == 49) s_bg = 0;
                    }
                    s_ansi_state = ANSI_STATE_NORMAL;
                } else if (ch == 'A') {
                    int d = s_csi_params[0] ? s_csi_params[0] : 1;
                    s_cur_y = (s_cur_y >= d) ? s_cur_y - d : 0;
                    s_ansi_state = ANSI_STATE_NORMAL;
                } else if (ch == 'B') {
                    int d = s_csi_params[0] ? s_csi_params[0] : 1;
                    s_cur_y = (s_cur_y + d < vh) ? s_cur_y + d : vh - 1;
                    s_ansi_state = ANSI_STATE_NORMAL;
                } else if (ch == 'C') {
                    int d = s_csi_params[0] ? s_csi_params[0] : 1;
                    s_cur_x = (s_cur_x + d < vw) ? s_cur_x + d : vw - 1;
                    s_ansi_state = ANSI_STATE_NORMAL;
                } else if (ch == 'D') {
                    int d = s_csi_params[0] ? s_csi_params[0] : 1;
                    s_cur_x = (s_cur_x >= d) ? s_cur_x - d : 0;
                    s_ansi_state = ANSI_STATE_NORMAL;
                } else if (ch >= 0x40 && ch <= 0x7E) {
                    /* Other command terminators (like 'h', 'l') */
                    s_ansi_state = ANSI_STATE_NORMAL;
                }
                break;
        }
    }
}

void terminate_osdep(void) {
    if (s_vga_saved) {
        volatile uint16_t *vga = VGA_MEMORY;
        if (vga && s_saved_vga) {
            for (size_t i = 0; i < s_saved_vga_cells; i++) {
                vga[i] = s_saved_vga[i];
            }
        }
        if (s_saved_vga) {
            free(s_saved_vga);
            s_saved_vga = NULL;
        }
        s_saved_vga_cells = 0;
        s_vga_saved = 0;
    }
}

int is_xterm(void) { return 0; }
int is_screen(void) { return 0; }
int is_twterm(void) { return 0; }
int resize_window(int x, int y) { (void)x; (void)y; return 0; }

static unsigned char *s_clipboard_buf = NULL;

unsigned char *get_clipboard_text(struct terminal *term) {
    (void)term;
    if (!s_clipboard_buf) return NULL;
    return stracpy(s_clipboard_buf);
}

void set_clipboard_text(struct terminal *term, unsigned char *text) {
    (void)term;
    if (s_clipboard_buf) {
        free(s_clipboard_buf);
        s_clipboard_buf = NULL;
    }
    if (text && *text) {
        size_t len = strlen((const char *)text);
        s_clipboard_buf = (unsigned char *)malloc(len + 1);
        if (s_clipboard_buf) memcpy(s_clipboard_buf, text, len + 1);
    }
}

int clipboard_support(struct terminal *term) {
    (void)term;
    return 1;
}

void set_window_title(unsigned char *title) { (void)title; }

unsigned char *get_window_title(void) {
    static unsigned char title[] = "Links";
    return title;
}
