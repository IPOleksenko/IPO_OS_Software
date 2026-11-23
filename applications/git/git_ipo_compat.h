#ifndef _GIT_IPO_COMPAT_H
#define _GIT_IPO_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifndef NSIG
#define NSIG 32
#endif

#ifndef SA_RESTART
#define SA_RESTART 0x10000000
#endif

#ifndef ITIMER_REAL
#define ITIMER_REAL 0
#endif

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

#ifndef _SC_PAGESIZE
#define _SC_PAGESIZE 30
#endif
#ifndef _SC_PAGE_SIZE
#define _SC_PAGE_SIZE 30
#endif
#ifndef _SC_OPEN_MAX
#define _SC_OPEN_MAX 4
#endif
#ifndef _SC_CLK_TCK
#define _SC_CLK_TCK 2
#endif
#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif

#ifdef __cplusplus
extern "C" {
#endif

mode_t umask(mode_t mask);
int fchmod(int fd, mode_t mode);
int chown(const char *pathname, uid_t owner, gid_t group);
int lchown(const char *pathname, uid_t owner, gid_t group);
int link(const char *oldpath, const char *newpath);
uid_t geteuid(void);
gid_t getegid(void);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
int setpgid(pid_t pid, pid_t pgid);
pid_t setsid(void);
int gethostname(char *name, size_t len);
unsigned int alarm(unsigned int seconds);
int utimes(const char *filename, const struct timeval times[2]);
void tzset(void);
char *ctermid(char *s);
pid_t tcgetpgrp(int fd);
int tcsetpgrp(int fd, pid_t pgrp);
int killpg(int pgrp, int sig);
long sysconf(int name);
int mkstemp(char *tmpl);
char *getpass(const char *prompt);
int faccessat(int dirfd, const char *pathname, int mode, int flags);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *string);
int execve(const char *pathname, char *const argv[], char *const envp[]);

struct repository;
const char *ipo_remote_curl_setup_git_dir(struct repository *repo, int *nongit_ok);
void ipo_remote_curl_trace2_cmd_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* _GIT_IPO_COMPAT_H */
