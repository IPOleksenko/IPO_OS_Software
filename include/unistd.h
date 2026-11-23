#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#ifdef __cplusplus
extern "C" {
#endif

extern char **environ;

int close(int fd);
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
off_t lseek(int fd, off_t offset, int whence);
int unlink(const char *pathname);
int rmdir(const char *pathname);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int access(const char *pathname, int mode);
int isatty(int fd);
pid_t getpid(void);
pid_t getppid(void);
uid_t getuid(void);
gid_t getgid(void);
unsigned int sleep(unsigned int seconds);
int usleep(useconds_t usec);
void *sbrk(intptr_t increment);
int brk(void *addr);

int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);

pid_t fork(void);
int pipe(int pipefd[2]);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int fsync(int fd);
int ftruncate(int fd, off_t length);
int truncate(const char *path, off_t length);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);
int symlink(const char *target, const char *linkpath);

#ifndef _SC_CLK_TCK
#define _SC_CLK_TCK          2
#endif
#ifndef _SC_OPEN_MAX
#define _SC_OPEN_MAX         4
#endif
#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE         30
#endif
#ifndef _SC_PAGE_SIZE
#define _SC_PAGE_SIZE        30
#endif
#ifndef _SC_NPROCESSORS_CONF
#define _SC_NPROCESSORS_CONF 83
#endif
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif

long sysconf(int name);

#ifdef __cplusplus
}
#endif

#endif
