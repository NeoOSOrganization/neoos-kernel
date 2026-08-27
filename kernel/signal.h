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

// Linux's x86-64 signal frame layout, verbatim -- musl's ucontext_t and
// its SA_SIGINFO handlers read these directly.
struct sigcontext_64 {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;          // user pointer to the FP area
    uint64_t reserved1[8];
};

struct ucontext_k {
    uint64_t             uc_flags;
    uint64_t             uc_link;
    stack_t_k            uc_stack;
    struct sigcontext_64 uc_mcontext;
    sigset_t_k           uc_sigmask;
    uint8_t              _sigmask_pad[128 - sizeof(sigset_t_k)];
};

struct rt_sigframe {
    uint64_t          pretcode;    // -> sa_restorer
    struct ucontext_k uc;
    struct siginfo    info;
};

// The FXSAVE area is allocated SEPARATELY, above the frame, and reached
// through uc.uc_mcontext.fpstate. It cannot be a member: the frame's own
// address must be 8 mod 16 so the handler sees rsp % 16 == 8 as if
// reached by `call`, which would put every 16-aligned member at 8 mod 16
// and make fxsave #GP. Linux separates it for the same reason.
// The extended-state milestone widens this and adds the xstate header.
#define SIGFRAME_FPSTATE_SIZE  512
#define SIGFRAME_FPSTATE_ALIGN 64

struct syscall_frame;
struct registers;
struct process;
struct thread;

int64_t signal_deliver_from_syscall(struct syscall_frame *f, int64_t num,
                                    int64_t retval);
void    signal_do_sigreturn(struct syscall_frame *f) __attribute__((noreturn));
void    signal_do_stop(struct thread *t, int sig);
void    signal_do_continue(struct process *p);
void    signal_raise_fault(struct registers *regs, int sig, int code, uint64_t addr);
void    signal_deliver_from_interrupt(struct registers *regs);
void    signal_terminate(struct process *p, int sig) __attribute__((noreturn));

// Default actions
#define SIGACT_TERM 0
#define SIGACT_IGN  1
#define SIGACT_CORE 2   // treated as TERM; NeoOS has no core dumps
#define SIGACT_STOP 3
#define SIGACT_CONT 4

int signal_default_action(int sig);

struct process;
struct thread;

// Standard signals (1-31) are not queued: one pending bit each, repeat
// sends collapse. RT signals (32-64) queue with their payloads, FIFO
// per signal number. The pool is fixed because NeoOS has no swap and an
// unbounded queue is a denial-of-service surface; exhaustion returns
// -EAGAIN, which POSIX permits.
#define SIGQUEUE_POOL 64

struct sigqueue {
    struct sigqueue *next;
    struct siginfo   info;
};

void signal_queue_init(void);
void sigqueue_release(struct sigqueue *q);

// Fills a siginfo for a signal sent by a process.
void siginfo_user(struct siginfo *out, int sig, int sender_pid);

int signal_send_thread(struct thread *t, int sig, struct siginfo *info);
int signal_send_process(struct process *p, int sig, struct siginfo *info);

// Group targeting lives with proc_list, so these are implemented in
// kernel/sched/proc.c.
int signal_kill(int pid, int sig, struct siginfo *info);
int signal_tkill(int tgid, int tid, int sig, struct siginfo *info);

// Makes `t` reach a delivery point. Given a real body in a later task;
// a no-op until blocking calls become signal-aware.
void signal_wake_for_delivery(struct thread *t);
int  signal_pending_any(struct thread *t);

void signal_init_process(struct process *p);
void signal_init_thread(struct thread *t);

// Lowest-numbered pending, unblocked signal for `t`, preferring
// thread-directed over process-directed. Returns 0 if none.
int signal_next_deliverable(struct thread *t);

void signal_selftest(void);
void signal_selftest_start(void);

#endif
