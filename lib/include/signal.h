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

typedef uint64_t sigset_t;

int kill(int pid, int sig);
int tkill(int tid, int sig);
int tgkill(int tgid, int tid, int sig);

#endif
