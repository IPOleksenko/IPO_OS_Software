#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <stddef.h>
#ifndef _PID_T_DECLARED
typedef int pid_t;
#define _PID_T_DECLARED
#endif

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15

typedef int sig_atomic_t;
typedef unsigned long sigset_t;
typedef void (*sighandler_t)(int);

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    union {
        void *si_addr;
        struct {
            pid_t si_pid;
            unsigned int si_uid;
        } sender;
    };
} siginfo_t;

typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} stack_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPOLL   SIGIO

#define SI_USER   0
#define SI_KERNEL 128

#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000u

#define SS_ONSTACK 1
#define SS_DISABLE 2
#define MINSIGSTKSZ 2048u
#define SIGSTKSZ    8192u

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define FPE_INTDIV 1
#define FPE_INTOVF 2
#define FPE_FLTDIV 3
#define FPE_FLTOVF 4
#define FPE_FLTUND 5
#define FPE_FLTRES 6
#define FPE_FLTINV 7
#define FPE_FLTSUB 8


sighandler_t signal(int signum, sighandler_t handler);
int raise(int sig);
int kill(pid_t pid, int sig);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int signum);
int sigdelset(sigset_t *set, int signum);
int sigismember(const sigset_t *set, int signum);
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
int sigaltstack(const stack_t *ss, stack_t *old_ss);

#endif
