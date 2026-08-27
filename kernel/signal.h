#ifndef NEOOS_SIGNAL_H
#define NEOOS_SIGNAL_H

#include <stdint.h>

// Signal numbers are Linux's x86-64 numbers, verbatim. musl's own
// signal.h uses these values and its SA_SIGINFO handlers read the
// structures below directly, so any divergence would mean patching
// musl's headers -- emulation creeping into what must stay a pure
// translation shim. See docs/superpowers/specs/2026-08-27-roadmap-
// architecture-design.md.
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

#define SIGRTMIN 32   // musl: 32 = SIGCANCEL, 33 = SIGSYNCCALL
#define SIGRTMAX 64
#define NSIG     65   // signals 1..64; index 0 unused

// sa_flags
#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

// sigprocmask `how`
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// si_code values this kernel produces
#define SI_USER       0
#define SI_KERNEL     0x80
#define SI_QUEUE      (-1)
#define SEGV_MAPERR   1
#define SEGV_ACCERR   2
#define FPE_INTDIV    1
#define FPE_FLTINV    7
#define ILL_ILLOPC    1
#define BUS_ADRERR    2
#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

typedef uint64_t sigset_t_k;   // one bit per signal; bit (sig-1)

static inline sigset_t_k sigmask_of(int sig) { return 1ULL << (sig - 1); }
static inline int  sigset_test(sigset_t_k s, int sig) { return (s & sigmask_of(sig)) != 0; }
static inline void sigset_add(sigset_t_k *s, int sig)  { *s |= sigmask_of(sig); }
static inline void sigset_del(sigset_t_k *s, int sig)  { *s &= ~sigmask_of(sig); }

// SIGKILL and SIGSTOP can be neither caught nor blocked (POSIX).
#define SIGSET_UNBLOCKABLE (sigmask_of(SIGKILL) | sigmask_of(SIGSTOP))

// Linux's stack_t, for sigaltstack.
typedef struct { void *ss_sp; int ss_flags; int _pad; uint64_t ss_size; } stack_t_k;
#define SS_ONSTACK 1
#define SS_DISABLE 2

// Matches Linux's x86-64 rt_sigaction argument exactly.
struct k_sigaction {
    void        (*handler)(int);
    unsigned long flags;
    void        (*restorer)(void);
    sigset_t_k    mask;
};

// Linux's siginfo_t is 128 bytes. Only the leading fields and the few
// union members this kernel produces are named; the rest is padding so
// the size is right for userland.
struct siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    int _pad0;
    union {
        struct { int si_pid; unsigned si_uid; }                kill;
        struct { int si_pid; unsigned si_uid; int si_status; }  chld;
        struct { void *si_addr; }                               fault;
        struct { int si_pid; unsigned si_uid; void *si_value; } rt;
        char _pad[112];
    } fields;
};

// Default actions
#define SIGACT_TERM 0
#define SIGACT_IGN  1
#define SIGACT_CORE 2   // treated as TERM; NeoOS has no core dumps
#define SIGACT_STOP 3
#define SIGACT_CONT 4

int signal_default_action(int sig);

struct process;
struct thread;

void signal_init_process(struct process *p);
void signal_init_thread(struct thread *t);

// Lowest-numbered pending, unblocked signal for `t`, preferring
// thread-directed over process-directed. Returns 0 if none.
int signal_next_deliverable(struct thread *t);

void signal_selftest(void);
void signal_selftest_start(void);

#endif
