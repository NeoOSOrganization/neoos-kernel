#ifndef NEOOS_LIB_SIGNAL_H
#define NEOOS_LIB_SIGNAL_H

#include <stdint.h>

// Mirrors kernel/signal.h exactly -- the two trees share no headers, so
// these must be kept in sync by hand, like the syscall numbers.
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGSYS  31

#define SIGRTMIN 32
#define SIGRTMAX 64

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

typedef uint64_t sigset_t;

struct sigaction {
    void   (*sa_handler)(int);
    unsigned long sa_flags;
    sigset_t sa_mask;
};

// Installs a handler. The SA_RESTORER the kernel requires is filled in
// automatically -- callers never see it.
int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int raise(int sig);

int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);

typedef struct { void *ss_sp; int ss_flags; int _pad; unsigned long ss_size; } stack_t;
#define SS_ONSTACK 1
#define SS_DISABLE 2
int sigaltstack(const stack_t *ss, stack_t *old);

static inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
static inline int sigfillset(sigset_t *s)  { *s = ~0ULL; return 0; }
static inline int sigaddset(sigset_t *s, int sig) { *s |= 1ULL << (sig - 1); return 0; }
static inline int sigdelset(sigset_t *s, int sig) { *s &= ~(1ULL << (sig - 1)); return 0; }
static inline int sigismember(const sigset_t *s, int sig) {
    return (*s & (1ULL << (sig - 1))) != 0;
}

int kill(int pid, int sig);
int tkill(int tid, int sig);
int tgkill(int tgid, int tid, int sig);

#endif
