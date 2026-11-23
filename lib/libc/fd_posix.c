#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/select.h>
#include <stdarg.h>

extern int ipo_libc_fd_pipe(int pipefd[2]);
extern int ipo_libc_fd_dup(int oldfd);
extern int ipo_libc_fd_dup2(int oldfd, int newfd);
extern int ipo_libc_fd_fcntl(int fd, int cmd, long arg);
extern int ipo_libc_fd_poll(struct pollfd *fds, nfds_t nfds, int timeout);
extern int ipo_libc_fd_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
                              struct timeval *timeout);

__attribute__((weak)) int pipe(int pipefd[2]) {
    return ipo_libc_fd_pipe(pipefd);
}

__attribute__((weak)) int dup(int oldfd) {
    return ipo_libc_fd_dup(oldfd);
}

__attribute__((weak)) int dup2(int oldfd, int newfd) {
    return ipo_libc_fd_dup2(oldfd, newfd);
}

__attribute__((weak)) int fcntl(int fd, int cmd, ...) {
    long arg = 0;
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC || cmd == F_SETFD || cmd == F_SETFL) {
        va_list ap;
        va_start(ap, cmd);
        arg = va_arg(ap, long);
        va_end(ap);
    }
    return ipo_libc_fd_fcntl(fd, cmd, arg);
}

__attribute__((weak)) int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return ipo_libc_fd_poll(fds, nfds, timeout);
}

__attribute__((weak)) int select(int nfds, fd_set *readfds, fd_set *writefds,
                                 fd_set *exceptfds, struct timeval *timeout) {
    return ipo_libc_fd_select(nfds, readfds, writefds, exceptfds, timeout);
}
