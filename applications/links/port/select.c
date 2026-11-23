#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <net/socket.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syscall.h>

extern int ipo_links_socket_connected(int fd);
extern int ipo_pipe_has_data(int fd);
extern int ipo_pipe_has_space(int fd);
extern int ipo_stdin_has_data(void);

#ifdef GRDRV_IPO
extern void ipo_gui_tick(void);
extern int ipo_gui_consume_wakeup(void);
#endif

static void poll_network(void) {
    ipo_syscall(IPO_SYSCALL_NET_POLL, 0u, NULL);
}

static void yield_process(void) {
    ipo_syscall(IPO_SYSCALL_PROCESS_YIELD, 0u, NULL);
}

static uint32_t milliseconds(void) {
    return (uint32_t)ipo_syscall(IPO_SYSCALL_MILLIS, 0u, NULL);
}

static int socket_readable(int fd) {
    unsigned char byte;
    return recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT) >= 0;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout) {
    fd_set requested_read, requested_write;
    uint32_t timeout_ms = 0;
    uint32_t start_ms = 0;
    int has_timeout = timeout != NULL;

    if (nfds < 0 || nfds > FD_SETSIZE ||
        (has_timeout && (timeout->tv_sec < 0 || timeout->tv_usec < 0))) {
        errno = EINVAL;
        return -1;
    }
    if (readfds) requested_read = *readfds;
    if (writefds) requested_write = *writefds;
    if (has_timeout) {
        uint64_t duration = (uint64_t)timeout->tv_sec * 1000u +
                            ((uint64_t)timeout->tv_usec + 999u) / 1000u;
        timeout_ms = duration > UINT32_MAX ? UINT32_MAX : (uint32_t)duration;
        start_ms = milliseconds();
    }

    for (;;) {
        int ready = 0;
        if (readfds) FD_ZERO(readfds);
        if (writefds) FD_ZERO(writefds);
        if (exceptfds) FD_ZERO(exceptfds);

        poll_network();

        for (int fd = 0; fd < nfds; fd++) {
            int r_ready = 0;
            int w_ready = 0;

            if (readfds && FD_ISSET(fd, &requested_read)) {
                if (fd == 0 && ipo_stdin_has_data()) {
                    r_ready = 1;
                } else if (ipo_pipe_has_data(fd)) {
                    r_ready = 1;
                } else if (fd >= 64 && socket_readable(fd)) {
                    r_ready = 1;
                }
            }

            if (writefds && FD_ISSET(fd, &requested_write)) {
                if (ipo_pipe_has_space(fd)) {
                    w_ready = 1;
                } else if (fd >= 64 && ipo_links_socket_connected(fd)) {
                    w_ready = 1;
                }
            }

            if (r_ready && readfds) {
                FD_SET(fd, readfds);
                ready++;
            }
            if (w_ready && writefds) {
                FD_SET(fd, writefds);
                ready++;
            }
        }

        if (ready)
            return ready;

#ifdef GRDRV_IPO
        ipo_gui_tick();
        if (ipo_gui_consume_wakeup()) {
            return 0;
        }
#endif

        if (has_timeout &&
            (uint32_t)(milliseconds() - start_ms) >= timeout_ms)
            return 0;

        yield_process();
    }
}
