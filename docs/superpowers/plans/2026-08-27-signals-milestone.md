# Signals Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** POSIX signals, job control, and `wait4`, so a user-mode fault kills only its process and musl's `abort()`/`raise()`/pthread cancellation have something to call.

**Architecture:** Dispositions per process, masks per thread, pending sets in both. Delivery builds Linux's `rt_sigframe` on the user stack at two points — the syscall exit path and interrupt return to ring 3 — and `rt_sigreturn` returns through an `iretq` trampoline because `sysret` cannot restore an arbitrary interrupted context.

**Tech Stack:** C (gnu11, freestanding, `-mcmodel=kernel`), NASM, x86-64, GRUB/Multiboot2, QEMU for verification.

**Spec:** `docs/superpowers/specs/2026-08-27-signals-milestone-design.md`
**Roadmap:** `docs/superpowers/specs/2026-08-27-roadmap-architecture-design.md`

## Global Constraints

- **No host test runner.** Bare-metal code with no host runtime. Every verification is an in-kernel selftest or a userland test program, run under headless QEMU, checked by grepping the serial log. Never write or propose host unit tests.
- **QEMU never exits on its own.** Always wrap it in `timeout`.
- **Every verification needs a fresh disk image.** `fat16_write_selftest` creates `/NEWDIR`, so a stale image reports `FAILED` on the second and later boots. Always `rm -f build/disk.img build/disk2.img` before `make disk-image`. This has caused false alarms twice in this project's history.
- **Work happens directly on `main`.** No feature branches (`CLAUDE.md`).
- **Standard-library convention is binding** (`CLAUDE.md`): any kernel feature reachable from user mode ships with a `lib/` wrapper **and** a `docs/stdlib.md` update in the same task. musl is not here yet, so these are real `lib/` wrappers.
- **Syscall numbers and shared structs are duplicated by hand** between `kernel/` and `lib/` — the two trees share no headers. Anything added to one must be added to the other in the same task.
- **Layouts are Linux's, verbatim.** `sigset_t`, `struct k_sigaction`, `siginfo_t`, `struct sigcontext`, `ucontext_t`, and the `W*` status encoding must match x86-64 Linux exactly. musl reads these structures directly; diverging means patching musl's headers later, which is emulation creeping into what must stay a translation shim.
- QEMU line for this milestone:
  `-cpu Nehalem -boot order=d -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw -display none -no-reboot`

### The "boot log UNCHANGED" check

Tasks 3 and 10 are refactors that must not change observable behavior.
Byte-identical comparison does **not** work — two runs of the identical
binary differ by ~57 lines, because preemption interleaves differently
and `task exited` lines reorder relative to test output. Filter the
timing-dependent lines and compare the **sorted multiset**:

```bash
filter() {
  grep -vE '^\[timer\] tick=|calibrated lapic|kmain address=|free_frames=|^\[(looper|yielder) pid=[0-9]+\] tick$' "$1" | sort
}

# BEFORE starting such a task:
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/baseline.log
filter /tmp/baseline.log > /tmp/baseline.txt

# after the task, same run into /tmp/after.log, then:
filter /tmp/after.log > /tmp/after.txt
diff /tmp/baseline.txt /tmp/after.txt && echo "IDENTICAL"
```

This gate was validated in the threads milestone: with this filter two
runs of one binary differ by **0** lines. `free_frames=` is filtered
because changing `.bss` moves it — check any delta is explainable
rather than ignoring it.

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `kernel/signal.h` / `signal.c` | sigset ops, dispositions, send/queue, selection, delivery, sigreturn |
| `kernel/sigframe.asm` | the `iretq` trampoline `rt_sigreturn` returns through |
| `lib/include/signal.h` / `lib/signal.c` | userland API plus the mandatory restorer |
| `lib/include/sys/wait.h` / `lib/wait.c` | `wait4`, `waitpid`, `W*` macros |
| `userland/sigtest.c` | the milestone's proof program |

**Modified:** `kernel/isr.h`, `kernel/isr.c`, `kernel/syscall.c`, `kernel/waitq.h`, `kernel/waitq.c`, `kernel/timer.h`, `kernel/timer.c`, `kernel/sched/proc.h`, `kernel/sched/proc.c`, `kernel/sched/thread.c`, `kernel/sched/sched.c`, `kernel/kernel.c`, `lib/syscall.c`, `lib/include/errno.h`, `docs/stdlib.md`, `Makefile`.

---

## Task 1: The signal data model

**Files:**
- Create: `kernel/signal.h`, `kernel/signal.c`
- Modify: `kernel/sched/proc.h`, `kernel/sched/proc.c`, `kernel/sched/thread.c`, `kernel/kernel.c`

**Interfaces:**
- Produces: `sigset_t_k` and its helpers; `struct k_sigaction`; `NSIG`/`SIGRTMIN` and every `SIG*` constant; `struct siginfo`; `int signal_default_action(int sig)`; `int signal_next_deliverable(struct thread *t)`; `void signal_init_process(struct process *p)`; `void signal_init_thread(struct thread *t)`; `void signal_selftest(void)`.
- Consumes: `struct process`, `struct thread`, `struct spinlock`.

No delivery, no sending. Just the model and its selection logic, which
is the part that is testable without user mode.

- [ ] **Step 1: Create `kernel/signal.h`**

```c
#ifndef NEOOS_SIGNAL_H
#define NEOOS_SIGNAL_H

#include <stdint.h>

// Signal numbers are Linux's x86-64 numbers, verbatim. musl's own
// signal.h uses these values and its SA_SIGINFO handlers read the
// structures below directly, so any divergence would mean patching
// musl's headers -- emulation creeping into the translation shim.
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
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000
#define SA_RESTORER  0x04000000

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)

// sigprocmask `how`
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

// si_code values this kernel produces
#define SI_USER    0
#define SI_KERNEL  0x80
#define SI_QUEUE   (-1)
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define FPE_INTDIV  1
#define FPE_FLTINV  7
#define ILL_ILLOPC  1
#define BUS_ADRERR  2
#define CLD_EXITED  1
#define CLD_KILLED  2
#define CLD_STOPPED 5
#define CLD_CONTINUED 6

typedef uint64_t sigset_t_k;   // one bit per signal; bit (sig-1)

// Linux's stack_t, for sigaltstack.
typedef struct { void *ss_sp; int ss_flags; int _pad; uint64_t ss_size; } stack_t_k;
#define SS_ONSTACK 1
#define SS_DISABLE 2

static inline sigset_t_k sigmask_of(int sig) { return 1ULL << (sig - 1); }
static inline int  sigset_test(sigset_t_k s, int sig) { return (s & sigmask_of(sig)) != 0; }
static inline void sigset_add(sigset_t_k *s, int sig)  { *s |= sigmask_of(sig); }
static inline void sigset_del(sigset_t_k *s, int sig)  { *s &= ~sigmask_of(sig); }

// SIGKILL and SIGSTOP can be neither caught nor blocked (POSIX).
#define SIGSET_UNBLOCKABLE (sigmask_of(SIGKILL) | sigmask_of(SIGSTOP))

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
        struct { int si_pid; unsigned si_uid; }               kill;
        struct { int si_pid; unsigned si_uid; int si_status; } chld;
        struct { void *si_addr; }                              fault;
        struct { int si_pid; unsigned si_uid; void *si_value; } rt;
        char _pad[112];
    } fields;
};

// Default actions
#define SIGACT_TERM 0
#define SIGACT_IGN  1
#define SIGACT_CORE 2   // treated as TERM; no core dumps in NeoOS
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

#endif
```

- [ ] **Step 2: Add the fields to `kernel/sched/proc.h`**

Add `#include "../signal.h"`, then to `struct process`:

```c
    struct k_sigaction actions[NSIG];   /* shared by all threads */
    struct spinlock    sig_lock;
    sigset_t_k         pending;         /* process-directed: kill() */
    struct sigqueue   *queued;          /* RT payloads (Task 2) */
    int                pgid, sid;
    int                exit_signal;     /* 0, or the signal that killed it */
    int                stopped_count;   /* threads parked in THREAD_STOPPED */
```

and to `struct thread`:

```c
    sigset_t_k       blocked;
    sigset_t_k       pending;           /* thread-directed: tkill() */
    struct sigqueue *queued;
    sigset_t_k       saved_blocked;     /* sigsuspend / sigreturn */
    int              in_sigsuspend;
    stack_t_k        altstack;          /* sigaltstack; zeroed = none */
```

Add `struct sigqueue;` as a forward declaration and
`#define LOCK_RANK_SIGNAL 2` is **not** needed — reuse
`LOCK_RANK_THREAD`'s neighbourhood by giving `sig_lock` rank
`LOCK_RANK_PROCESS`. It is only ever taken alone.

- [ ] **Step 3: Create `kernel/signal.c` with the model and selection**

```c
#include "signal.h"
#include "sched/proc.h"
#include "serial.h"

int signal_default_action(int sig) {
    switch (sig) {
    case SIGCHLD:
        return SIGACT_IGN;
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return SIGACT_STOP;
    case SIGCONT:
        return SIGACT_CONT;
    case SIGQUIT: case SIGILL: case SIGABRT: case SIGFPE:
    case SIGSEGV: case SIGBUS: case SIGSYS: case SIGTRAP:
        return SIGACT_CORE;   // NeoOS has no core dumps; behaves as TERM
    default:
        return SIGACT_TERM;   // includes every RT signal
    }
}

void signal_init_process(struct process *p) {
    spin_init(&p->sig_lock, LOCK_RANK_PROCESS, "signal");
    for (int i = 0; i < NSIG; i++) {
        p->actions[i].handler  = SIG_DFL;
        p->actions[i].flags    = 0;
        p->actions[i].restorer = 0;
        p->actions[i].mask     = 0;
    }
    p->pending = 0;
    p->queued  = 0;
    p->exit_signal   = 0;
    p->stopped_count = 0;
}

void signal_init_thread(struct thread *t) {
    t->blocked       = 0;
    t->pending       = 0;
    t->queued        = 0;
    t->saved_blocked = 0;
    t->in_sigsuspend = 0;
}

int signal_next_deliverable(struct thread *t) {
    struct process *p = t->proc;
    // Thread-directed first, then process-directed; lowest number wins
    // within each. SIGKILL and SIGSTOP are never blockable, so they are
    // always visible here.
    sigset_t_k ready = t->pending & ~t->blocked;
    if (!ready && p) { ready = p->pending & ~t->blocked; }
    if (!ready) { return 0; }
    for (int sig = 1; sig < NSIG; sig++) {
        if (ready & sigmask_of(sig)) { return sig; }
    }
    return 0;
}
```

- [ ] **Step 4: Initialise from the allocators**

In `kernel/sched/proc.c`'s `proc_alloc`, after `waitq_init(&p->join_waiters);`:

```c
    signal_init_process(p);
    // Inherited by fork/spawn in Task 10; a first process leads its own
    // group and session.
    p->pgid = p->pid;
    p->sid  = p->pid;
```

In `kernel/sched/thread.c`'s `thread_alloc`, after
`cpu_default_fpu_state(t->fpu_state);`:

```c
    signal_init_thread(t);
```

- [ ] **Step 5: Add the selftest**

Append to `signal.c`:

```c
void signal_selftest(void) {
    sigset_t_k s = 0;
    sigset_add(&s, SIGSEGV);
    sigset_add(&s, SIGRTMIN);
    if (!sigset_test(s, SIGSEGV) || !sigset_test(s, SIGRTMIN)) {
        serial_write_string("[signal] selftest FAILED: add/test\n");
        return;
    }
    if (sigset_test(s, SIGKILL)) {
        serial_write_string("[signal] selftest FAILED: spurious member\n");
        return;
    }
    sigset_del(&s, SIGSEGV);
    if (sigset_test(s, SIGSEGV)) {
        serial_write_string("[signal] selftest FAILED: del\n");
        return;
    }
    // Bit 63 must be reachable: SIGRTMAX is signal 64.
    sigset_t_k top = sigmask_of(SIGRTMAX);
    if (top != (1ULL << 63)) {
        serial_write_string("[signal] selftest FAILED: SIGRTMAX bit\n");
        return;
    }
    if (signal_default_action(SIGCHLD) != SIGACT_IGN ||
        signal_default_action(SIGSTOP) != SIGACT_STOP ||
        signal_default_action(SIGCONT) != SIGACT_CONT ||
        signal_default_action(SIGTERM) != SIGACT_TERM ||
        signal_default_action(SIGRTMIN) != SIGACT_TERM) {
        serial_write_string("[signal] selftest FAILED: default actions\n");
        return;
    }

    // Selection order, exercised on the current thread.
    struct thread *t = current_thread();
    sigset_t_k save_p = t->pending, save_b = t->blocked;
    t->pending = 0; t->blocked = 0;
    sigset_add(&t->pending, SIGUSR2);
    sigset_add(&t->pending, SIGUSR1);
    if (signal_next_deliverable(t) != SIGUSR1) {
        serial_write_string("[signal] selftest FAILED: lowest-first\n");
        t->pending = save_p; t->blocked = save_b; return;
    }
    sigset_add(&t->blocked, SIGUSR1);
    if (signal_next_deliverable(t) != SIGUSR2) {
        serial_write_string("[signal] selftest FAILED: blocked not skipped\n");
        t->pending = save_p; t->blocked = save_b; return;
    }
    sigset_add(&t->blocked, SIGUSR2);
    if (signal_next_deliverable(t) != 0) {
        serial_write_string("[signal] selftest FAILED: all blocked\n");
        t->pending = save_p; t->blocked = save_b; return;
    }
    t->pending = save_p; t->blocked = save_b;

    serial_write_string("[signal] selftest passed\n");
}
```

Call `signal_selftest();` from `kmain` **after** the spawns, alongside
`waitq_selftest_start()` — it reads `current_thread()`, and placing it
before the spawns would shift every pid.

Because `kmain` is not a thread, wrap it the same way `waitq` does:
add a one-line kernel thread in `signal.c`:

```c
static void signal_selftest_thread(void) { signal_selftest(); thread_exit_self(0); }
void signal_selftest_start(void) { thread_alloc_kernel(signal_selftest_thread); }
```

and declare `void signal_selftest_start(void);` in `signal.h`.

- [ ] **Step 6: Build and verify**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -n "\[signal\]" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: `[signal] selftest passed`, zero `FAILED`/exceptions, and the
existing boot otherwise unchanged.

- [ ] **Step 7: Commit**

```bash
git add kernel/signal.h kernel/signal.c kernel/sched/proc.h \
        kernel/sched/proc.c kernel/sched/thread.c kernel/kernel.c
git commit -m "Add the signal data model: sigsets, dispositions, and delivery selection"
```

---

## Task 2: Sending — `kill`, `tkill`, `tgkill`, and the RT queue

**Files:**
- Modify: `kernel/signal.h`, `kernel/signal.c`, `kernel/syscall.c`, `lib/syscall.c`

**Interfaces:**
- Consumes: Task 1's model.
- Produces: `int signal_send_thread(struct thread *t, int sig, struct siginfo *info)`; `int signal_send_process(struct process *p, int sig, struct siginfo *info)`; syscalls `SYS_KILL` (29), `SYS_TKILL` (30), `SYS_TGKILL` (31).

Signals accumulate as pending; nothing is delivered yet. That is
deliberate — a send bug and a delivery bug look identical in a log, so
they are separated by a task boundary.

- [ ] **Step 1: Add the RT queue to `signal.h`**

```c
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

int signal_send_thread(struct thread *t, int sig, struct siginfo *info);
int signal_send_process(struct process *p, int sig, struct siginfo *info);

// Fills a siginfo for a signal sent by a process.
void siginfo_user(struct siginfo *out, int sig, int sender_pid);
```

- [ ] **Step 2: Implement sending in `signal.c`**

```c
static struct sigqueue sigqueue_pool[SIGQUEUE_POOL];
static struct sigqueue *sigqueue_free;
static struct spinlock  sigqueue_lock;

void signal_queue_init(void) {
    spin_init(&sigqueue_lock, LOCK_RANK_PROCESS, "sigqueue");
    sigqueue_free = 0;
    for (int i = 0; i < SIGQUEUE_POOL; i++) {
        sigqueue_pool[i].next = sigqueue_free;
        sigqueue_free = &sigqueue_pool[i];
    }
}

static struct sigqueue *sigqueue_alloc(void) {
    uint64_t f = spin_lock_irqsave(&sigqueue_lock);
    struct sigqueue *q = sigqueue_free;
    if (q) { sigqueue_free = q->next; q->next = 0; }
    spin_unlock_irqrestore(&sigqueue_lock, f);
    return q;
}

void sigqueue_release(struct sigqueue *q) {
    uint64_t f = spin_lock_irqsave(&sigqueue_lock);
    q->next = sigqueue_free;
    sigqueue_free = q;
    spin_unlock_irqrestore(&sigqueue_lock, f);
}

void siginfo_user(struct siginfo *out, int sig, int sender_pid) {
    for (unsigned i = 0; i < sizeof(*out); i++) { ((uint8_t *)out)[i] = 0; }
    out->si_signo = sig;
    out->si_code  = SI_USER;
    out->fields.kill.si_pid = sender_pid;
}

// Appends `info` to `*head` in FIFO order. Only RT signals queue.
static int queue_append(struct sigqueue **head, struct siginfo *info) {
    struct sigqueue *q = sigqueue_alloc();
    if (!q) { return -EAGAIN; }
    q->info = *info;
    q->next = 0;
    while (*head) { head = &(*head)->next; }
    *head = q;
    return 0;
}

int signal_send_thread(struct thread *t, int sig, struct siginfo *info) {
    if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
    struct process *p = t->proc;
    if (!p) { return -ESRCH; }

    // An ignored signal is discarded at send time rather than pending
    // forever -- except SIGKILL/SIGSTOP, which cannot be ignored.
    if (!(sigmask_of(sig) & SIGSET_UNBLOCKABLE)) {
        void (*h)(int) = p->actions[sig].handler;
        if (h == SIG_IGN ||
            (h == SIG_DFL && signal_default_action(sig) == SIGACT_IGN)) {
            return 0;
        }
    }

    uint64_t f = spin_lock_irqsave(&p->sig_lock);
    if (sig >= SIGRTMIN && info) {
        int rc = queue_append(&t->queued, info);
        if (rc != 0) { spin_unlock_irqrestore(&p->sig_lock, f); return rc; }
    }
    sigset_add(&t->pending, sig);
    spin_unlock_irqrestore(&p->sig_lock, f);

    signal_wake_for_delivery(t);   // Task 6 gives this a body
    return 0;
}

int signal_send_process(struct process *p, int sig, struct siginfo *info) {
    if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
    if (!(sigmask_of(sig) & SIGSET_UNBLOCKABLE)) {
        void (*h)(int) = p->actions[sig].handler;
        if (h == SIG_IGN ||
            (h == SIG_DFL && signal_default_action(sig) == SIGACT_IGN)) {
            return 0;
        }
    }

    uint64_t f = spin_lock_irqsave(&p->sig_lock);
    if (sig >= SIGRTMIN && info) {
        int rc = queue_append(&p->queued, info);
        if (rc != 0) { spin_unlock_irqrestore(&p->sig_lock, f); return rc; }
    }
    sigset_add(&p->pending, sig);
    spin_unlock_irqrestore(&p->sig_lock, f);

    // Deliver to any one thread that does not block it; prefer one that
    // is already awake so a sleeping thread is not woken unnecessarily.
    struct thread *target = 0;
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        if (!sigset_test(t->blocked, sig)) {
            target = t;
            if (t->state != THREAD_BLOCKED) { break; }
        }
    }
    if (target) { signal_wake_for_delivery(target); }
    return 0;
}
```

Add a temporary no-op so this compiles before Task 6:

```c
void signal_wake_for_delivery(struct thread *t) { (void)t; }
```

Call `signal_queue_init();` from `signal_selftest_start()`'s caller —
better, from `process_init()` in `kernel/sched/proc.c`, since the pool
must exist before any send.

- [ ] **Step 3: Add the three syscalls to `kernel/syscall.c`**

```c
#define SYS_KILL   29
#define SYS_TKILL  30
#define SYS_TGKILL 31
```

```c
        case SYS_KILL: {
            int pid = (int)a1, sig = (int)a2;
            struct siginfo info;
            siginfo_user(&info, sig, current_proc()->pid);
            return signal_kill(pid, sig, &info);
        }
        case SYS_TKILL: {
            int tid = (int)a1, sig = (int)a2;
            struct siginfo info;
            siginfo_user(&info, sig, current_proc()->pid);
            return signal_tkill(0, tid, sig, &info);
        }
        case SYS_TGKILL: {
            struct siginfo info;
            siginfo_user(&info, (int)a3, current_proc()->pid);
            return signal_tkill((int)a1, (int)a2, (int)a3, &info);
        }
```

- [ ] **Step 4: Implement `signal_kill` and `signal_tkill`**

In `signal.c`. Group targeting (`pid <= 0`) needs `proc_list`, so this
lives where `proc_lock` is visible — declare both in `signal.h` and
implement in `kernel/sched/proc.c`, which already includes `sched.h`:

```c
// pid  > 0  that process
// pid == 0  every process in the caller's group
// pid <  -1 every process in group -pid
// pid == -1 every process the caller may signal
int signal_kill(int pid, int sig, struct siginfo *info) {
    if (sig < 0 || sig >= NSIG) { return -EINVAL; }
    if (pid > 0) {
        struct process *p = proc_find(pid);
        if (!p || p->state == PROC_ZOMBIE) { return -ESRCH; }
        return sig ? signal_send_process(p, sig, info) : 0;  // sig 0 = probe
    }

    int target_pgid = (pid == 0) ? current_proc()->pgid : -pid;
    int found = 0, rc = 0;
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (struct process *p = proc_list; p; p = p->next) {
        if (p->state == PROC_ZOMBIE) { continue; }
        if (pid != -1 && p->pgid != target_pgid) { continue; }
        found = 1;
        if (sig) {
            int r = signal_send_process(p, sig, info);
            if (r != 0) { rc = r; }
        }
    }
    spin_unlock_irqrestore(&proc_lock, f);
    return found ? rc : -ESRCH;
}

int signal_tkill(int tgid, int tid, int sig, struct siginfo *info) {
    if (sig < 0 || sig >= NSIG) { return -EINVAL; }
    uint64_t f = spin_lock_irqsave(&proc_lock);
    for (struct process *p = proc_list; p; p = p->next) {
        if (tgid > 0 && p->pid != tgid) { continue; }
        for (struct thread *t = p->threads; t; t = t->proc_next) {
            if (t->tid != tid) { continue; }
            spin_unlock_irqrestore(&proc_lock, f);
            return sig ? signal_send_thread(t, sig, info) : 0;
        }
    }
    spin_unlock_irqrestore(&proc_lock, f);
    return -ESRCH;
}
```

**Lock-order hazard:** `signal_send_process` takes `p->sig_lock`
(rank `LOCK_RANK_PROCESS` = 1) while `proc_lock` (rank
`LOCK_RANK_PROCTABLE` = 0) is held. That is ascending, so it is legal —
the rank checker will confirm. Do **not** reverse it anywhere.

- [ ] **Step 5: Add the raw wrappers to `lib/syscall.c`**

```c
#define SYS_KILL   29
#define SYS_TKILL  30
#define SYS_TGKILL 31

int kill(int pid, int sig)              { return (int)syscall2(SYS_KILL, pid, sig); }
int tkill(int tid, int sig)             { return (int)syscall2(SYS_TKILL, tid, sig); }
int tgkill(int tgid, int tid, int sig)  { return (int)syscall3(SYS_TGKILL, tgid, tid, sig); }
```

Declare them in `lib/include/signal.h` — created fully in Task 4; for
now a minimal header with just these three plus the `SIG*` numbers is
enough.

- [ ] **Step 6: Build and verify**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
grep -n "\[signal\]\|vfstest\] ALL\|child exit code" /tmp/neoos.log
```

Expected: unchanged behavior — nothing sends signals yet. Zero
`FAILED`/exceptions. A `[lock] PANIC: rank inversion` here means the
`proc_lock`/`sig_lock` ordering above was reversed somewhere.

- [ ] **Step 7: Commit**

```bash
git add kernel/signal.h kernel/signal.c kernel/sched/proc.c kernel/sched/proc.h \
        kernel/syscall.c lib/syscall.c lib/include/signal.h
git commit -m "Add signal sending with RT queueing and kill/tkill/tgkill"
```

---

## Task 3: Extend `struct registers`

**Files:**
- Modify: `kernel/isr.h`

**Interfaces:**
- Produces: `regs->rsp`, `regs->ss`.

Tiny but isolated on purpose: it changes the layout of a struct that
assembly writes, and a mistake here corrupts every interrupt. Gated on
the boot log.

- [ ] **Step 1: Capture the baseline** (see Global Constraints).

- [ ] **Step 2: Add the two fields**

In `kernel/isr.h`:

```c
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector_number;
    uint64_t error_code;
    uint64_t rip, cs, rflags;
    // In long mode the CPU pushes SS:RSP at EVERY privilege level, so
    // these have always been on the stack -- the struct simply stopped
    // at rflags. Signal delivery from a fault needs the user RSP to
    // build a frame on. isr.asm needs no change: it never touched them.
    uint64_t rsp, ss;
} __attribute__((packed));
```

`kernel/isr.asm` is **not** modified. Its `add rsp, 16` and `iretq`
already skip past the CPU-pushed frame correctly; the struct was merely
under-declared.

- [ ] **Step 3: Prove the fields hold real values**

Temporarily extend `exception_dump_and_halt`'s output in
`kernel/isr.c`:

```c
    serial_write_string(" rsp="); serial_write_hex64(regs->rsp);
    serial_write_string(" ss=");  serial_write_hex64(regs->ss);
```

Temporarily replace `kmain`'s `spawn("/BIN/THRDTEST.ELF");` with
`spawn("/BIN/FAULTER.ELF");`, build, and run. Expected: the exception
dump reports `ss=0x33` (the ring-3 data selector set by
`kernel_thread_trampoline`) and an `rsp` just below `0x700000000000`
(the user stack top). Anything else — `ss=0`, or an `rsp` that looks
like a kernel address — means the fields are misaligned.

Then revert both temporary edits.

- [ ] **Step 4: Verify the log is unchanged**

Run the gate from Global Constraints. Expected: `IDENTICAL`.

- [ ] **Step 5: Commit**

```bash
git add kernel/isr.h
git commit -m "Declare the SS and RSP the CPU already pushes into struct registers"
```

---

## Task 4: Frame construction, `rt_sigreturn`, and first delivery

**Files:**
- Create: `kernel/sigframe.asm`, `lib/include/signal.h` (full), `lib/signal.c`
- Modify: `kernel/signal.h`, `kernel/signal.c`, `kernel/syscall.c`, `lib/syscall.c`, `Makefile`, `userland/sigtest.c` (created here, minimal)

**Interfaces:**
- Produces: `void signal_deliver_from_syscall(struct syscall_frame *f)`; `void signal_do_sigreturn(struct syscall_frame *f)` (noreturn); `SYS_RT_SIGACTION` (21), `SYS_RT_SIGRETURN` (23); the userland `sigaction`, `raise`, and the restorer.

The core of the milestone.

- [ ] **Step 1: Add the frame structures to `kernel/signal.h`**

Layouts are Linux's, verbatim.

```c
struct sigcontext_64 {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;          /* user pointer to the FP area */
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
    uint64_t            pretcode;    /* -> sa_restorer */
    struct ucontext_k   uc;
    struct siginfo      info;
    /* FXSAVE area follows; uc.uc_mcontext.fpstate points at it.
       The extended-state milestone must widen this and add the xstate
       header -- a consequence of taking signals first. */
    uint8_t             fpstate[512] __attribute__((aligned(16)));
};
```

- [ ] **Step 2: Create `kernel/sigframe.asm`**

```nasm
; kernel/sigframe.asm -- returns to user mode with a fully arbitrary
; register state, which rt_sigreturn requires and the ordinary sysret
; epilogue cannot provide: sysretq takes RIP from RCX and RFLAGS from
; R11 and refuses a non-canonical RIP, so it cannot faithfully restore a
; context that was interrupted by preemption rather than by a syscall.

section .text
[bits 64]
global sigreturn_to_user

; void sigreturn_to_user(struct iret_ctx *ctx) -- never returns.
; struct iret_ctx is laid out exactly in pop order, ending with the
; five-qword frame iretq consumes.
sigreturn_to_user:
    mov rsp, rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ; [rsp] now holds RIP, CS, RFLAGS, RSP, SS -- iretq's own frame.
    ; GS still names the per-CPU block; hand it back to userland first.
    swapgs
    iretq
```

Add to the `Makefile`'s `ASM_OBJECTS` and give it a rule mirroring
`fork_trampoline.o`:

```makefile
$(BUILD_DIR)/sigframe.o: kernel/sigframe.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/sigframe.asm -o $(BUILD_DIR)/sigframe.o
```

- [ ] **Step 3: Build the frame and enter the handler**

In `signal.c`:

```c
struct iret_ctx {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};
extern void sigreturn_to_user(struct iret_ctx *ctx) __attribute__((noreturn));

#define USER_CS 0x3B
#define USER_SS 0x33

// Copies the interrupted context into `sc` and pushes a complete
// rt_sigframe onto the user stack. Returns the new user RSP, or 0 if
// the stack could not take the frame.
static uint64_t build_frame(struct thread *t, int sig,
                            struct siginfo *info,
                            struct sigcontext_64 *sc,
                            sigset_t_k oldmask,
                            const struct k_sigaction *ka) {
    uint64_t sp = sc->rsp;

    // SA_ONSTACK: run on the alternate stack if one is installed and we
    // are not already on it. This is what makes a SIGSEGV handler
    // survivable after a stack overflow -- without it the handler
    // re-faults on the same guard page.
    if ((ka->flags & SA_ONSTACK) && t->altstack.ss_size &&
        !(sp >= (uint64_t)(uintptr_t)t->altstack.ss_sp &&
          sp <  (uint64_t)(uintptr_t)t->altstack.ss_sp + t->altstack.ss_size)) {
        sp = (uint64_t)(uintptr_t)t->altstack.ss_sp + t->altstack.ss_size;
    }

    sp -= sizeof(struct rt_sigframe);
    sp &= ~0xFULL;              // 16-byte align the frame itself
    sp -= 8;                    // so the handler sees rsp % 16 == 8,
                                // exactly as if reached by `call`

    struct rt_sigframe *fr = (struct rt_sigframe *)(uintptr_t)sp;
    if (!user_range_writable((uint64_t)(uintptr_t)fr, sizeof(*fr))) {
        return 0;
    }

    for (unsigned i = 0; i < sizeof(*fr); i++) { ((uint8_t *)fr)[i] = 0; }

    fr->pretcode          = (uint64_t)(uintptr_t)ka->restorer;
    fr->uc.uc_mcontext    = *sc;
    fr->uc.uc_sigmask     = oldmask;
    fr->uc.uc_stack       = t->altstack;
    fr->uc.uc_mcontext.fpstate = (uint64_t)(uintptr_t)fr->fpstate;
    fpu_save(fr->fpstate);
    if (info) { fr->info = *info; }

    return sp;
}
```

`user_range_writable(uint64_t addr, uint64_t len)` does not exist. Add
it to `kernel/mm/paging.c`/`paging.h`: walk the current PML4 and
confirm every page in the range is present, user-accessible and
writable. It returns 0 for a bad range, which is what turns a bad
`sigaltstack` into a clean kill rather than a kernel fault.

- [ ] **Step 4: The delivery entry point for the syscall path**

```c
// Applies `sig` to the interrupted context described by `sc`, entering
// the handler. Returns 1 if a handler was entered (caller must write
// `sc` back), 0 if the signal was handled by a default action that does
// not return here (the process was terminated).
static int deliver_one(struct thread *t, int sig, struct siginfo *info,
                       struct sigcontext_64 *sc) {
    struct process *p = t->proc;
    struct k_sigaction ka = p->actions[sig];

    if (ka.handler == SIG_IGN) { return 1; }          // nothing to do
    if (ka.handler == SIG_DFL) {
        switch (signal_default_action(sig)) {
        case SIGACT_IGN:  return 1;
        case SIGACT_STOP: signal_do_stop(t, sig); return 1;   /* Task 9 */
        case SIGACT_CONT: return 1;                            /* Task 9 */
        default:
            p->exit_signal = sig;
            process_exit(0);        // never returns
        }
    }

    // SA_RESTORER is mandatory on x86-64, as on Linux: the kernel never
    // injects a trampoline onto the user stack. lib/signal.c fills it in
    // for every sigaction(), and musl supplies its own.
    if (!(ka.flags & SA_RESTORER) || !ka.restorer) {
        p->exit_signal = SIGSEGV;
        process_exit(0);
    }

    sigset_t_k oldmask = t->blocked;
    uint64_t sp = build_frame(t, sig, info, sc, oldmask, &ka);
    if (!sp) {                       // unwritable stack: die, do not fault
        p->exit_signal = SIGSEGV;
        process_exit(0);
    }

    // Block sa_mask (plus this signal unless SA_NODEFER) for the handler.
    t->blocked |= ka.mask;
    if (!(ka.flags & SA_NODEFER)) { sigset_add(&t->blocked, sig); }
    t->blocked &= ~SIGSET_UNBLOCKABLE;

    if (ka.flags & SA_RESETHAND) { p->actions[sig].handler = SIG_DFL; }

    // Enter the handler: SysV first argument is the signal number, and
    // for SA_SIGINFO the second and third are siginfo and ucontext.
    sc->rip = (uint64_t)(uintptr_t)ka.handler;
    sc->rsp = sp;
    sc->rdi = (uint64_t)sig;
    if (ka.flags & SA_SIGINFO) {
        struct rt_sigframe *fr = (struct rt_sigframe *)(uintptr_t)sp;
        sc->rsi = (uint64_t)(uintptr_t)&fr->info;
        sc->rdx = (uint64_t)(uintptr_t)&fr->uc;
    }
    // Direction flag must be clear on entry to a SysV function.
    sc->eflags &= ~(1ULL << 10);
    return 1;
}
```

- [ ] **Step 5: Wire delivery into the syscall exit point**

`struct syscall_frame`'s field order is fixed by `syscall_entry.asm`;
convert both ways rather than aliasing.

```c
static void sc_from_syscall(struct sigcontext_64 *sc, struct syscall_frame *f,
                            int64_t retval) {
    sc->r8  = f->r8;  sc->r9  = f->r9;  sc->r10 = f->r10;
    sc->r11 = f->r11; sc->r12 = f->r12; sc->r13 = f->r13;
    sc->r14 = f->r14; sc->r15 = f->r15;
    sc->rdi = f->rdi; sc->rsi = f->rsi; sc->rbp = f->rbp;
    sc->rbx = f->rbx; sc->rdx = f->rdx; sc->rcx = f->rcx;
    sc->rax = (uint64_t)retval;
    sc->rsp = f->user_rsp;
    sc->rip = f->rcx;       // syscall_entry saved user RIP in rcx
    sc->eflags = f->r11;    // and user RFLAGS in r11
    sc->cs = USER_CS; sc->ss = USER_SS;
}

static void sc_to_syscall(struct syscall_frame *f, struct sigcontext_64 *sc) {
    f->r8  = sc->r8;  f->r9  = sc->r9;  f->r10 = sc->r10;
    f->r12 = sc->r12; f->r13 = sc->r13; f->r14 = sc->r14; f->r15 = sc->r15;
    f->rdi = sc->rdi; f->rsi = sc->rsi; f->rbp = sc->rbp;
    f->rbx = sc->rbx; f->rdx = sc->rdx;
    f->rcx = sc->rip;       // sysret takes user RIP from rcx
    f->r11 = sc->eflags;    // and RFLAGS from r11
    f->user_rsp = sc->rsp;
}

// Called from syscall_dispatch's single exit point. Returns the value
// the syscall should return (unchanged unless a handler was entered).
int64_t signal_deliver_from_syscall(struct syscall_frame *f, int64_t retval) {
    struct thread *t = current_thread();
    if (!t || !t->proc) { return retval; }

    int sig;
    while ((sig = signal_next_deliverable(t)) != 0) {
        struct siginfo info;
        signal_take_pending(t, sig, &info);     /* clears the bit, pops any queue entry */

        struct sigcontext_64 sc;
        sc_from_syscall(&sc, f, retval);
        deliver_one(t, sig, &info, &sc);
        sc_to_syscall(f, &sc);
        retval = (int64_t)sc.rax;
    }
    return retval;
}
```

`signal_take_pending(struct thread *t, int sig, struct siginfo *out)`
clears the pending bit — from the thread set if present there, else the
process set — and pops the head of the matching RT queue into `*out`,
releasing it to the pool. For a standard signal with no queued entry it
synthesises `SI_USER` with `si_pid = 0`.

In `kernel/syscall.c`, replace the `kill_pending` check:

```c
int64_t syscall_dispatch(int64_t num, int64_t a1, int64_t a2, int64_t a3,
                         int64_t a4, struct syscall_frame *frame) {
    int64_t ret = syscall_dispatch_inner(num, a1, a2, a3, a4, frame);
    return signal_deliver_from_syscall(frame, ret);
}
```

The `kill_pending` field and its check are removed in Task 6, once
`SIGKILL` replaces them. Until then, leave the existing check in place
**after** the delivery call.

- [ ] **Step 6: Implement `rt_sigreturn`**

```c
// Restores the context saved in the frame the handler is standing on.
// Never returns: it leaves through an iretq trampoline, because sysret
// cannot restore an arbitrary interrupted context.
void signal_do_sigreturn(struct syscall_frame *f) {
    struct thread *t = current_thread();

    // The handler's own RSP is 8 past the frame (its `ret` popped
    // pretcode), so the frame starts one qword below.
    uint64_t sp = f->user_rsp - 8;
    struct rt_sigframe *fr = (struct rt_sigframe *)(uintptr_t)sp;
    if (!user_range_writable((uint64_t)(uintptr_t)fr, sizeof(*fr))) {
        t->proc->exit_signal = SIGSEGV;
        process_exit(0);
    }

    t->blocked = fr->uc.uc_sigmask & ~SIGSET_UNBLOCKABLE;
    t->altstack = fr->uc.uc_stack;
    fpu_restore(fr->fpstate);

    struct sigcontext_64 *sc = &fr->uc.uc_mcontext;

    struct iret_ctx ctx;
    ctx.r15 = sc->r15; ctx.r14 = sc->r14; ctx.r13 = sc->r13; ctx.r12 = sc->r12;
    ctx.r11 = sc->r11; ctx.r10 = sc->r10; ctx.r9 = sc->r9;   ctx.r8  = sc->r8;
    ctx.rbp = sc->rbp; ctx.rdi = sc->rdi; ctx.rsi = sc->rsi; ctx.rdx = sc->rdx;
    ctx.rcx = sc->rcx; ctx.rbx = sc->rbx; ctx.rax = sc->rax;
    ctx.rip = sc->rip; ctx.cs = USER_CS;
    // Force IF on and clear the bits userland must not choose.
    ctx.rflags = (sc->eflags & 0x3C7FD7ULL) | (1ULL << 9) | 2ULL;
    ctx.rsp = sc->rsp; ctx.ss = USER_SS;

    // `ctx` is a local on this thread's kernel stack, BELOW the syscall
    // frame at the top -- so the trampoline's `mov rsp, rdi` cannot
    // clobber anything still needed.
    sigreturn_to_user(&ctx);
}
```

Dispatch case:

```c
        case SYS_RT_SIGRETURN:
            signal_do_sigreturn(frame);
            return 0; // unreachable
```

- [ ] **Step 7: Implement `rt_sigaction`**

```c
        case SYS_RT_SIGACTION: {
            int sig = (int)a1;
            const struct k_sigaction *act = (const struct k_sigaction *)(uintptr_t)a2;
            struct k_sigaction *old = (struct k_sigaction *)(uintptr_t)a3;
            if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
            // SIGKILL and SIGSTOP can be neither caught nor ignored.
            if (act && (sig == SIGKILL || sig == SIGSTOP)) { return -EINVAL; }

            struct process *p = current_proc();
            uint64_t f = spin_lock_irqsave(&p->sig_lock);
            if (old) { *old = p->actions[sig]; }
            if (act) {
                p->actions[sig] = *act;
                p->actions[sig].mask &= ~SIGSET_UNBLOCKABLE;
            }
            spin_unlock_irqrestore(&p->sig_lock, f);
            return 0;
        }
```

- [ ] **Step 8: Create `lib/include/signal.h` and `lib/signal.c`**

`lib/include/signal.h` duplicates the `SIG*`, `SA_*`, `SIG_*`
constants, `sigset_t`, `struct sigaction`, and declares `sigaction`,
`signal`, `raise`, `kill`, `tkill`, and the `sigset_t` helpers.

`lib/signal.c`:

```c
#include <signal.h>
#include <errno.h>

#define SYS_RT_SIGACTION 21
#define SYS_RT_SIGRETURN 23

// SA_RESTORER is mandatory on x86-64: the kernel never injects a
// trampoline onto the user stack, so every sigaction() must supply
// one. Callers never see it.
__asm__(
    ".globl __restore_rt\n"
    ".hidden __restore_rt\n"
    "__restore_rt:\n"
    "   mov $23, %eax\n"       /* SYS_RT_SIGRETURN */
    "   syscall\n"
);
extern void __restore_rt(void);

struct k_sigaction {
    void        (*handler)(int);
    unsigned long flags;
    void        (*restorer)(void);
    unsigned long mask;
};

extern long __sys_rt_sigaction(int sig, const struct k_sigaction *act,
                               struct k_sigaction *old);

int sigaction(int sig, const struct sigaction *act, struct sigaction *old) {
    struct k_sigaction ka, ko;
    if (act) {
        ka.handler  = act->sa_handler;
        ka.flags    = act->sa_flags | SA_RESTORER;
        ka.restorer = __restore_rt;
        ka.mask     = act->sa_mask;
    }
    long rc = __sys_rt_sigaction(sig, act ? &ka : 0, old ? &ko : 0);
    if (rc == 0 && old) {
        old->sa_handler = ko.handler;
        old->sa_flags   = ko.flags & ~SA_RESTORER;
        old->sa_mask    = ko.mask;
    }
    return (int)rc;
}

int raise(int sig) { return tkill(thread_self(), sig); }
```

- [ ] **Step 9: Write a minimal `userland/sigtest.c` and its Makefile rule**

```c
#include <unistd.h>
#include <stdio.h>
#include <signal.h>

static volatile int got;
static volatile int handler_arg;

static void handler(int sig) { got = 1; handler_arg = sig; }

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct sigaction sa;
    sa.sa_handler = handler;
    sa.sa_flags   = 0;
    sa.sa_mask    = 0;
    if (sigaction(SIGUSR1, &sa, 0) != 0) {
        printf("[sigtest] FAILED: sigaction\n");
        return 1;
    }

    // A value the handler must not disturb: proves sigreturn restored
    // the interrupted context rather than approximating it.
    volatile long canary = 0x0123456789ABCDEFL;
    got = 0;
    raise(SIGUSR1);

    if (!got)                        { printf("[sigtest] FAILED: handler did not run\n"); return 1; }
    if (handler_arg != SIGUSR1)      { printf("[sigtest] FAILED: wrong signal %d\n", handler_arg); return 1; }
    if (canary != 0x0123456789ABCDEFL) { printf("[sigtest] FAILED: context clobbered\n"); return 1; }

    printf("[sigtest] handler + sigreturn passed\n");
    printf("[sigtest] ALL PASSED\n");
    return 0;
}
```

Makefile rule named `SIGTEST.ELF` (8.3-safe), added to `$(DISK_IMG)`
prerequisites and `mcopy` lines, mirroring `THRDTEST.ELF`. Spawn it
from `kmain` after `THRDTEST`.

- [ ] **Step 10: Build and verify**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -n "sigtest" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: `handler + sigreturn passed`, `[sigtest] ALL PASSED`, and the
process exiting normally. Zero `FAILED`/exceptions.

**If the boot triple-faults or the process dies immediately**, suspect
in this order: (1) the frame alignment — the handler must see
`rsp % 16 == 8`; (2) `pretcode` not pointing at `__restore_rt`; (3) the
`iret_ctx` field order disagreeing with `sigframe.asm`'s pop sequence.

- [ ] **Step 11: Commit**

```bash
git add kernel/sigframe.asm kernel/signal.h kernel/signal.c kernel/syscall.c \
        kernel/mm/paging.h kernel/mm/paging.c lib/include/signal.h lib/signal.c \
        lib/syscall.c userland/sigtest.c Makefile kernel/kernel.c
git commit -m "Deliver signals to handlers and return through an iretq trampoline"
```

---

## Task 5: Masks — `sigprocmask`, `sigpending`, `sigsuspend`

**Files:**
- Modify: `kernel/signal.c`, `kernel/syscall.c`, `lib/signal.c`, `lib/include/signal.h`, `userland/sigtest.c`

**Interfaces:**
- Produces: `SYS_RT_SIGPROCMASK` (22), `SYS_RT_SIGPENDING` (24), `SYS_RT_SIGSUSPEND` (25).

- [ ] **Step 1: Add the three dispatch cases**

```c
        case SYS_RT_SIGPROCMASK: {
            int how = (int)a1;
            const sigset_t_k *set = (const sigset_t_k *)(uintptr_t)a2;
            sigset_t_k *old = (sigset_t_k *)(uintptr_t)a3;
            struct thread *t = current_thread();
            if (old) { *old = t->blocked; }
            if (set) {
                sigset_t_k v = *set;
                switch (how) {
                case SIG_BLOCK:   t->blocked |= v; break;
                case SIG_UNBLOCK: t->blocked &= ~v; break;
                case SIG_SETMASK: t->blocked = v; break;
                default: return -EINVAL;
                }
                // SIGKILL and SIGSTOP are silently dropped from any mask,
                // as POSIX requires -- not an error.
                t->blocked &= ~SIGSET_UNBLOCKABLE;
            }
            return 0;
        }
        case SYS_RT_SIGPENDING: {
            sigset_t_k *out = (sigset_t_k *)(uintptr_t)a1;
            struct thread *t = current_thread();
            if (out) { *out = (t->pending | t->proc->pending) & t->blocked; }
            return 0;
        }
        case SYS_RT_SIGSUSPEND: {
            const sigset_t_k *mask = (const sigset_t_k *)(uintptr_t)a1;
            struct thread *t = current_thread();
            t->saved_blocked = t->blocked;
            t->in_sigsuspend = 1;
            t->blocked = (*mask) & ~SIGSET_UNBLOCKABLE;
            // Sleep until a signal is deliverable. The delivery path
            // restores saved_blocked (see below), so this always returns
            // -EINTR after running a handler.
            while (signal_next_deliverable(t) == 0) {
                if (waitq_sleep(&t->proc->sig_waiters, 0) == -EINTR) { break; }
            }
            return -EINTR;
        }
```

`sigpending` reports pending-and-blocked, which is what POSIX
specifies: an unblocked pending signal would already have been
delivered.

Add `struct waitq sig_waiters;` to `struct process`, initialised in
`proc_alloc`, and have `signal_wake_for_delivery` wake it (Task 6).

- [ ] **Step 2: Restore the mask after a `sigsuspend` handler**

In `deliver_one`, immediately before entering the handler:

```c
    // sigsuspend's temporary mask must not survive into the frame the
    // handler will sigreturn from, or the original mask is lost.
    if (t->in_sigsuspend) {
        oldmask = t->saved_blocked;
        t->in_sigsuspend = 0;
    }
```

placed **before** `build_frame` is called, so the frame records the
pre-`sigsuspend` mask.

- [ ] **Step 3: Library wrappers**

`sigprocmask`, `sigpending`, `sigsuspend`, plus `sigemptyset`,
`sigfillset`, `sigaddset`, `sigdelset`, `sigismember` in
`lib/include/signal.h` / `lib/signal.c`.

- [ ] **Step 4: Extend `sigtest.c`**

```c
static int check_masking(void) {
    struct sigaction sa;
    sa.sa_handler = handler; sa.sa_flags = 0; sa.sa_mask = 0;
    sigaction(SIGUSR2, &sa, 0);

    sigset_t block = 0, old = 0, pend = 0;
    sigaddset(&block, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block, &old);

    got = 0;
    raise(SIGUSR2);
    if (got) { printf("[sigtest] FAILED: blocked signal delivered\n"); return 0; }

    sigpending(&pend);
    if (!sigismember(&pend, SIGUSR2)) {
        printf("[sigtest] FAILED: sigpending missed it\n"); return 0;
    }

    sigprocmask(SIG_SETMASK, &old, 0);
    if (!got) { printf("[sigtest] FAILED: unblock did not deliver\n"); return 0; }

    printf("[sigtest] blocking + sigpending passed\n");
    return 1;
}
```

Note what this proves: the signal must be delivered **by the
`sigprocmask` syscall's own return path**, since that is the first
delivery point reached after unblocking.

- [ ] **Step 5: Build, verify, commit**

Expected: `blocking + sigpending passed` alongside the Task 4 line.

```bash
git add kernel/signal.c kernel/signal.h kernel/syscall.c kernel/sched/proc.h \
        kernel/sched/proc.c lib/signal.c lib/include/signal.h userland/sigtest.c
git commit -m "Add per-thread signal masks, sigpending, and sigsuspend"
```

---

## Task 6: Interruption, `SA_RESTART`, and retiring `kill_pending`

**Files:**
- Modify: `kernel/signal.c`, `kernel/waitq.c`, `kernel/waitq.h`, `kernel/sched/proc.h`, `kernel/sched/proc.c`, `kernel/sched/thread.c`, `kernel/syscall.c`, `userland/sigtest.c`

**Interfaces:**
- Produces: `void signal_wake_for_delivery(struct thread *t)` (real body); `int signal_pending_any(struct thread *t)`.

- [ ] **Step 1: Give `signal_wake_for_delivery` a body**

```c
// A thread with a deliverable signal must reach a delivery point. If it
// is blocked in an interruptible sleep, wake it: waitq_sleep will see
// the pending signal and return -EINTR.
void signal_wake_for_delivery(struct thread *t) {
    if (t->state == THREAD_BLOCKED && signal_next_deliverable(t)) {
        waitq_remove(t);
        t->state = THREAD_READY;
        thread_enqueue_ready(t);
    }
}

int signal_pending_any(struct thread *t) {
    return signal_next_deliverable(t) != 0;
}
```

- [ ] **Step 2: Make `waitq_sleep` signal-aware**

In `kernel/waitq.c`, replace both `kill_pending` tests:

```c
    if (signal_pending_any(t)) {
        if (!release && (own_flags & (1ULL << 9))) { __asm__ volatile ("sti"); }
        return -EINTR;
    }
```

and after `schedule()`:

```c
    int rc = signal_pending_any(t) ? -EINTR : 0;
```

- [ ] **Step 3: Replace `kill_pending` with `SIGKILL`**

In `kernel/sched/thread.c`, `thread_kill` becomes:

```c
// Terminates `t`. SIGKILL cannot be caught, blocked or ignored, so this
// is now just a send -- the delivery path does the rest. The threads
// milestone's kill_pending flag was a one-signal prototype of exactly
// this, and keeping both would mean two ways to kill a thread and two
// checks on the syscall path.
void thread_kill(struct thread *t) {
    if (t->state == THREAD_ZOMBIE) { return; }
    struct siginfo info;
    siginfo_user(&info, SIGKILL, 0);
    signal_send_thread(t, SIGKILL, &info);
}
```

Delete the `kill_pending` field from `struct thread` and every
reference: `waitq.c` (done above), `syscall.c`'s dispatch wrapper
(delete the leftover check from Task 4 Step 5), and `thread_create`'s
initialiser.

`process_exit` keeps its sibling loop — it now sends `SIGKILL` to each.

- [ ] **Step 4: Implement `SA_RESTART`**

`struct syscall_frame` has no `rax` field: the number arrives in `RAX`
and the return value leaves in it. So a restart is "rewind RIP past the
`syscall` instruction and put the number back in RAX", and the second
half is free — just return the number.

In `signal_deliver_from_syscall`, pass the syscall number in and, when
a handler is entered for a call that returned `-EINTR`:

```c
int64_t signal_deliver_from_syscall(struct syscall_frame *f, int64_t num,
                                    int64_t retval) {
    struct thread *t = current_thread();
    if (!t || !t->proc) { return retval; }

    int sig;
    while ((sig = signal_next_deliverable(t)) != 0) {
        struct siginfo info;
        signal_take_pending(t, sig, &info);
        int restart = (retval == -EINTR) &&
                      (t->proc->actions[sig].flags & SA_RESTART) &&
                      (t->proc->actions[sig].handler != SIG_DFL) &&
                      (t->proc->actions[sig].handler != SIG_IGN);

        struct sigcontext_64 sc;
        sc_from_syscall(&sc, f, retval);
        if (restart) {
            sc.rip -= 2;          // back onto the `syscall` instruction
            sc.rax = (uint64_t)num;
        }
        deliver_one(t, sig, &info, &sc);
        sc_to_syscall(f, &sc);
        retval = (int64_t)sc.rax;
    }
    return retval;
}
```

The rewind is 2 because `syscall` is `0F 05`. Update the caller in
`syscall.c` to pass `num`.

- [ ] **Step 5: Extend `sigtest.c`**

```c
#include <thread.h>

static volatile int join_rc;
static volatile int victim_returned;

// Never exits, so anything joining it blocks forever.
static void forever(void *arg) { (void)arg; for (;;) { yield(); } }

static thread_t block_target;

static void victim(void *arg) {
    (void)arg;
    join_rc = thread_join(block_target, 0);
    victim_returned = 1;
    thread_exit(0);
}

static volatile int usr1_count;
static void counting_handler(int sig) { (void)sig; usr1_count++; }

// Installs SIGUSR1 with the given flags, blocks a thread inside
// thread_join, signals it, and reports whether the join returned.
static int run_interrupt_case(int flags, int expect_return) {
    struct sigaction sa;
    sa.sa_handler = counting_handler;
    sa.sa_flags   = flags;
    sa.sa_mask    = 0;
    sigaction(SIGUSR1, &sa, 0);

    thread_create(&block_target, forever, 0);
    join_rc = 0;
    victim_returned = 0;

    thread_t v;
    thread_create(&v, victim, 0);
    for (volatile int i = 0; i < 2000000; i++) { }   // let it reach the join

    tkill(v, SIGUSR1);
    for (volatile int i = 0; i < 2000000; i++) { }   // let the handler run

    return victim_returned == expect_return;
}

static int check_eintr(void) {
    // No SA_RESTART: the interrupted thread_join must return -EINTR.
    if (!run_interrupt_case(0, 1)) {
        printf("[sigtest] FAILED: interrupted join did not return\n");
        return 0;
    }
    if (join_rc != -EINTR) {
        printf("[sigtest] FAILED: join returned %d, want -EINTR\n", join_rc);
        return 0;
    }
    printf("[sigtest] EINTR passed\n");

    // With SA_RESTART the call is restarted, so the join stays blocked
    // and the victim never returns.
    if (!run_interrupt_case(SA_RESTART, 0)) {
        printf("[sigtest] FAILED: SA_RESTART did not restart the call\n");
        return 0;
    }
    printf("[sigtest] SA_RESTART passed\n");
    return 1;
}
```

Both cases leak a `forever` thread and a blocked victim on purpose;
`exit()` kills them, which is itself a check that the threads
milestone's sibling teardown still works now that it goes through
`SIGKILL`.

- [ ] **Step 6: Build, verify, commit**

Expected: the existing sigtest lines plus `EINTR passed` and
`SA_RESTART passed`. Zero `FAILED`/exceptions, and the four-process
boot unchanged — `kill_pending`'s removal must not alter it.

```bash
git add kernel/signal.c kernel/signal.h kernel/waitq.c kernel/sched/proc.h \
        kernel/sched/thread.c kernel/syscall.c userland/sigtest.c
git commit -m "Interrupt blocking calls with signals and implement SA_RESTART"
```

---

## Task 7: Faults become signals, and `sigaltstack`

**Files:**
- Modify: `kernel/isr.c`, `kernel/signal.c`, `kernel/signal.h`, `kernel/syscall.c`, `lib/signal.c`, `lib/include/signal.h`, `kernel/kernel.c`, `userland/sigtest.c`

**Interfaces:**
- Produces: `void signal_raise_fault(struct registers *regs, int sig, int code, uint64_t addr)`; `SYS_SIGALTSTACK` (28).

- [ ] **Step 1: Add `sigaltstack`**

```c
        case SYS_SIGALTSTACK: {
            const stack_t_k *ss = (const stack_t_k *)(uintptr_t)a1;
            stack_t_k *old = (stack_t_k *)(uintptr_t)a2;
            struct thread *t = current_thread();
            if (old) { *old = t->altstack; }
            if (ss) {
                if (ss->ss_size < 2048) { return -ENOMEM; }
                t->altstack = *ss;
            }
            return 0;
        }
```

Add `#define ENOMEM 12` to both `errno.h` files, and mirror
`SS_ONSTACK`/`SS_DISABLE` (already in the kernel header from Task 1)
into `lib/include/signal.h`.

- [ ] **Step 2: Map ring-3 faults to signals in `isr.c`**

Replace the `if (regs->vector_number < 32)` block:

```c
    if (regs->vector_number < 32) {
        // A fault from ring 0 is a kernel bug and must still stop the
        // machine loudly. A fault from ring 3 kills only its process.
        if ((regs->cs & 3) != 3) {
            exception_dump_and_halt(regs);
            return;
        }

        uint64_t cr2 = 0;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        int sig = SIGSEGV, code = SI_KERNEL;
        uint64_t addr = 0;
        switch (regs->vector_number) {
        case 0:  sig = SIGFPE;  code = FPE_INTDIV;  break;
        case 6:  sig = SIGILL;  code = ILL_ILLOPC;  addr = regs->rip; break;
        case 13: sig = SIGSEGV; code = SI_KERNEL;   break;
        case 14: sig = SIGSEGV;
                 code = (regs->error_code & 1) ? SEGV_ACCERR : SEGV_MAPERR;
                 addr = cr2;
                 break;
        case 16: case 19: sig = SIGFPE; code = FPE_FLTINV; addr = regs->rip; break;
        case 17: sig = SIGBUS;  code = BUS_ADRERR; addr = cr2; break;
        default: break;
        }
        signal_raise_fault(regs, sig, code, addr);
        return;
    }
```

- [ ] **Step 3: Implement `signal_raise_fault`**

```c
// Raises a synchronous fault signal on the current thread and delivers
// it immediately: unlike an asynchronous signal there is nowhere to
// return to, since re-executing the faulting instruction would just
// fault again.
void signal_raise_fault(struct registers *regs, int sig, int code, uint64_t addr) {
    struct thread *t = current_thread();
    struct process *p = t ? t->proc : 0;
    if (!p) { exception_dump_and_halt(regs); return; }

    struct siginfo info;
    for (unsigned i = 0; i < sizeof(info); i++) { ((uint8_t *)&info)[i] = 0; }
    info.si_signo = sig;
    info.si_code  = code;
    info.fields.fault.si_addr = (void *)(uintptr_t)addr;

    // A fault signal that is blocked or ignored cannot be honoured:
    // POSIX says the behaviour is undefined, and every real kernel
    // force-delivers the default action. Otherwise the process would
    // spin re-faulting forever.
    if (sigset_test(t->blocked, sig) ||
        p->actions[sig].handler == SIG_IGN) {
        p->exit_signal = sig;
        process_exit(0);
    }

    struct sigcontext_64 sc;
    sc_from_registers(&sc, regs);
    deliver_one(t, sig, &info, &sc);
    sc_to_registers(regs, &sc);
}
```

`sc_from_registers` / `sc_to_registers` convert `struct registers` both
ways, using the `rsp`/`ss` fields Task 3 declared. `regs->err` and
`regs->trapno` go into `sc.err`/`sc.trapno`, and `cr2` into `sc.cr2`.

- [ ] **Step 4: Deliver asynchronous signals on interrupt return too**

Still in `isr_handler`, after the timer/keyboard handling, before
returning:

```c
    // Without this, a signal cannot interrupt a compute loop -- only a
    // thread that happens to make a syscall would ever notice one.
    if ((regs->cs & 3) == 3) {
        signal_deliver_from_interrupt(regs);
    }
```

`signal_deliver_from_interrupt` mirrors the syscall version but has no
return value to preserve and never restarts anything.

- [ ] **Step 5: `faulter` rejoins the standard boot**

In `kernel/kernel.c`, add `spawn("/BIN/FAULTER.ELF");` after the other
spawns. This is the milestone's headline: today it `cli; hlt`s the
kernel, which is why it has never been in the boot.

- [ ] **Step 6: Extend `sigtest.c` with the SIGSEGV and altstack checks**

```c
static char altstack[16384] __attribute__((aligned(16)));
static volatile int segv_seen;

static void segv_handler(int sig) { (void)sig; segv_seen = 1; _exit(77); }

static int check_segv(void) {
    stack_t ss = { .ss_sp = altstack, .ss_flags = 0, .ss_size = sizeof(altstack) };
    if (sigaltstack(&ss, 0) != 0) { printf("[sigtest] FAILED: sigaltstack\n"); return 0; }

    struct sigaction sa;
    sa.sa_handler = segv_handler;
    sa.sa_flags   = SA_ONSTACK;
    sa.sa_mask    = 0;
    sigaction(SIGSEGV, &sa, 0);

    // Deliberate null dereference. The handler runs on the alternate
    // stack and _exit()s, so this function never returns.
    volatile int *p = 0;
    *p = 1;
    printf("[sigtest] FAILED: null deref did not fault\n");
    return 0;
}
```

Run this **last** in `main`, from a forked child so the parent can
report the outcome via `wait`. Expected exit code 77.

- [ ] **Step 7: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -n "faulter\|sigtest\|task exited" /tmp/neoos.log
grep -c "exception" /tmp/neoos.log
```

Expected, and this is the milestone's clearest single result:
`faulter about to divide by zero`, then a `[process] task exited` line
for its pid — **and the other six processes carrying on to completion**.
Zero `exception` lines: `exception_dump_and_halt` must no longer run.

- [ ] **Step 8: Commit**

```bash
git add kernel/isr.c kernel/signal.c kernel/signal.h kernel/syscall.c \
        kernel/errno.h kernel/kernel.c lib/signal.c lib/include/signal.h \
        lib/include/errno.h userland/sigtest.c
git commit -m "Turn ring-3 faults into signals and add sigaltstack"
```

---

## Task 8: `THREAD_STOPPED`, `SIGSTOP`, `SIGCONT`

**Files:**
- Modify: `kernel/sched/proc.h`, `kernel/sched/sched.c`, `kernel/signal.c`

**Interfaces:**
- Produces: `THREAD_STOPPED`; `void signal_do_stop(struct thread *t, int sig)`; `void signal_do_continue(struct process *p)`.

- [ ] **Step 1: Add the scheduler state**

```c
enum thread_state { THREAD_UNUSED, THREAD_READY, THREAD_RUNNING,
                    THREAD_BLOCKED, THREAD_STOPPED, THREAD_ZOMBIE };
```

`schedule()` needs no change to *avoid* stopped threads — they are
never on the ready queue — but its "prev was RUNNING, re-enqueue it"
test must not re-enqueue a thread that just stopped. It already tests
`prev->state == THREAD_RUNNING`, and `signal_do_stop` sets
`THREAD_STOPPED` before calling `schedule()`, so this is correct as
written. Confirm it rather than changing it.

The idle thread introduced by the threads milestone is what makes this
safe: a process whose every thread stops no longer risks leaving the
CPU with nothing to run.

- [ ] **Step 2: Implement stop and continue**

```c
// Stop is process-wide: SIGSTOP marks every thread. Threads blocked in
// an interruptible sleep are woken so they reach a delivery point,
// where they park here instead of running a handler.
void signal_do_stop(struct thread *t, int sig) {
    struct process *p = t->proc;
    (void)sig;

    for (struct thread *o = p->threads; o; o = o->proc_next) {
        if (o != t && o->state != THREAD_ZOMBIE && o->state != THREAD_STOPPED) {
            sigset_add(&o->pending, SIGSTOP);
            signal_wake_for_delivery(o);
        }
    }

    uint64_t f = spin_lock_irqsave(&p->sig_lock);
    p->stopped_count++;
    // The process counts as stopped only when EVERY thread has parked;
    // reporting a half-complete group stop to wait4 would be a lie.
    int all_stopped = 1;
    for (struct thread *o = p->threads; o; o = o->proc_next) {
        if (o != t && o->state != THREAD_STOPPED && o->state != THREAD_ZOMBIE) {
            all_stopped = 0; break;
        }
    }
    spin_unlock_irqrestore(&p->sig_lock, f);

    // Wake the parent's wait4(WUNTRACED). child_waiters and
    // stop_reported arrive in Task 9; until then this line is
    // `(void)all_stopped;` and the stop is simply not reported.
    if (all_stopped) {
        p->stop_reported = 0;
        struct process *parent = proc_find(p->parent_pid);
        if (parent) { waitq_wake_all(&parent->child_waiters); }
    }

    __asm__ volatile ("cli");
    t->state = THREAD_STOPPED;
    __asm__ volatile ("sti");
    schedule();
    // Resumed by SIGCONT.
}

void signal_do_continue(struct process *p) {
    uint64_t f = spin_lock_irqsave(&p->sig_lock);
    sigset_del(&p->pending, SIGSTOP);
    p->stopped_count = 0;
    spin_unlock_irqrestore(&p->sig_lock, f);

    for (struct thread *t = p->threads; t; t = t->proc_next) {
        if (t->state == THREAD_STOPPED) {
            sigset_del(&t->pending, SIGSTOP);
            t->state = THREAD_READY;
            thread_enqueue_ready(t);
        }
    }
}
```

- [ ] **Step 3: Route `SIGCONT` at send time, not delivery time**

`SIGCONT` must resume a stopped process even though a stopped thread
never reaches a delivery point. In `signal_send_process` and
`signal_send_thread`, before queueing:

```c
    if (sig == SIGCONT) { signal_do_continue(p); }
    if (sigmask_of(sig) & (sigmask_of(SIGSTOP) | sigmask_of(SIGTSTP) |
                           sigmask_of(SIGTTIN) | sigmask_of(SIGTTOU))) {
        // A pending SIGCONT is discarded by a stop, and vice versa.
        sigset_del(&p->pending, SIGCONT);
    }
```

- [ ] **Step 4: Build and verify**

No userland test yet — `wait4(WUNTRACED)` arrives in Task 9. Verify the
existing boot is unchanged and nothing regressed.

- [ ] **Step 5: Commit**

```bash
git add kernel/sched/proc.h kernel/sched/sched.c kernel/signal.c
git commit -m "Add THREAD_STOPPED with process-wide SIGSTOP and SIGCONT"
```

---

## Task 9: `wait4`, status encoding, `SIGCHLD`, process groups

**Files:**
- Modify: `kernel/sched/proc.h`, `kernel/sched/proc.c`, `kernel/signal.c`, `kernel/syscall.c`, `lib/syscall.c`
- Create: `lib/include/sys/wait.h`, `lib/wait.c`

**Interfaces:**
- Produces: `int64_t wait4(int pid, int *status, int options)`; `SYS_WAIT4` (32), `SYS_SETPGID` (33), `SYS_GETPGID` (34), `SYS_SETSID` (35), `SYS_GETSID` (36).

This task is gated on the boot log: `SYS_WAIT` must keep behaving
exactly as it does today.

- [ ] **Step 1: Capture the baseline** (Global Constraints).

- [ ] **Step 2: Record how a process died**

`process_exit` already takes a code. Set `p->exit_signal` before
calling it wherever a signal kills the process (Task 4's `deliver_one`
and Task 7's `signal_raise_fault` already do). Add the encoder:

```c
// Linux-compatible wait status encoding.
static int encode_status(struct process *p) {
    if (p->exit_signal) { return p->exit_signal & 0x7f; }     /* signalled */
    return (p->exit_code & 0xff) << 8;                        /* exited */
}
#define STATUS_STOPPED(sig)  (0x7f | ((sig) << 8))
#define STATUS_CONTINUED     0xffff
```

- [ ] **Step 3: Implement `wait4`**

```c
int64_t wait4(int pid, int *status, int options) {
    struct process *self = current_proc();

    for (;;) {
        int found = 0;
        for (struct process *p = proc_list; p; p = p->next) {
            if (p->parent_pid != self->pid) { continue; }
            if (pid > 0  && p->pid  != pid)  { continue; }
            if (pid == 0 && p->pgid != self->pgid) { continue; }
            if (pid < -1 && p->pgid != -pid) { continue; }
            found = 1;

            if (p->state == PROC_ZOMBIE) {
                int st = encode_status(p);
                int reaped = p->pid;
                if (status) { *status = st; }
                proc_reap(p);          /* the existing wait_for_pid teardown */
                return reaped;
            }
            if ((options & WUNTRACED) && p->stopped_count > 0 && !p->stop_reported) {
                p->stop_reported = 1;
                if (status) { *status = STATUS_STOPPED(SIGSTOP); }
                return p->pid;
            }
        }
        if (!found) { return -ECHILD; }
        if (options & WNOHANG) { return 0; }
        if (waitq_sleep(&self->child_waiters, 0) == -EINTR) { return -EINTR; }
    }
}
```

Add `struct waitq child_waiters;` and `int stop_reported;` to
`struct process` — **Task 8 already referenced both**, so add them
there if that task came first; `proc_reap` is the zombie-thread-and-struct teardown
lifted out of the existing `wait_for_pid`. Add `#define ECHILD 10` to
both `errno.h` files.

- [ ] **Step 4: `SYS_WAIT` becomes a wrapper**

```c
        case SYS_WAIT: {
            // NeoOS-native: one pid, bare exit code. Kept beside POSIX
            // wait4 rather than replaced by it -- see docs/stdlib.md.
            int st = 0;
            int64_t rc = wait4((int)a1, &st, 0);
            if (rc < 0) { return rc; }
            // Same encoding as <sys/wait.h>'s WIFEXITED, spelled out
            // because the kernel does not include the userland header.
            return ((st & 0x7f) == 0) ? ((st >> 8) & 0xff) : -(st & 0x7f);
        }
```

Delete the old `wait_for_pid` body, keeping only `proc_reap`.

- [ ] **Step 5: `SIGCHLD` on child exit**

In `proc_put`, where the process becomes a zombie:

```c
    struct process *parent = proc_find(p->parent_pid);
    if (parent) {
        struct siginfo info;
        for (unsigned i = 0; i < sizeof(info); i++) { ((uint8_t *)&info)[i] = 0; }
        info.si_signo = SIGCHLD;
        info.si_code  = p->exit_signal ? CLD_KILLED : CLD_EXITED;
        info.fields.chld.si_pid    = p->pid;
        info.fields.chld.si_status = p->exit_signal ? p->exit_signal : p->exit_code;
        signal_send_process(parent, SIGCHLD, &info);
        waitq_wake_all(&parent->child_waiters);
    }
```

`SIGCHLD`'s default action is ignore, so this changes nothing for
processes that do not install a handler — but it is what makes a
`wait4(WNOHANG)` polling loop and `sigsuspend`-based reaping work.

- [ ] **Step 6: Process groups and sessions**

```c
        case SYS_SETPGID: {
            int pid = (int)a1 ? (int)a1 : current_proc()->pid;
            int pgid = (int)a2 ? (int)a2 : pid;
            struct process *p = proc_find(pid);
            if (!p) { return -ESRCH; }
            p->pgid = pgid;
            return 0;
        }
        case SYS_GETPGID: {
            struct process *p = (int)a1 ? proc_find((int)a1) : current_proc();
            return p ? p->pgid : -ESRCH;
        }
        case SYS_SETSID: {
            struct process *p = current_proc();
            p->sid = p->pid;
            p->pgid = p->pid;
            return p->sid;
        }
        case SYS_GETSID: {
            struct process *p = (int)a1 ? proc_find((int)a1) : current_proc();
            return p ? p->sid : -ESRCH;
        }
```

`fork` and `spawn` inherit `pgid`/`sid` from the caller — add those two
lines to both, in `kernel/sched/proc.c`.

- [ ] **Step 7: `lib/include/sys/wait.h` and `lib/wait.c`**

```c
#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0xff) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)
#define WIFCONTINUED(s) ((s) == 0xffff)

int wait4(int pid, int *status, int options, void *rusage);
int waitpid(int pid, int *status, int options);
```

- [ ] **Step 8: Verify the log is unchanged, then extend `sigtest.c`**

Run the gate first. `parent.c` uses `wait()` and must still print
`[parent] child exit code=42` — that line is the proof `SYS_WAIT` kept
its native behaviour.

Then add the job-control checks:

```c
#include <sys/wait.h>

static int check_stop_continue(void) {
    int child = fork();
    if (child < 0) { printf("[sigtest] FAILED: fork\n"); return 0; }
    if (child == 0) {
        for (volatile long i = 0; i < 200000000L; i++) { }
        _exit(5);
    }

    for (volatile int i = 0; i < 2000000; i++) { }   // let it start
    kill(child, SIGSTOP);

    int st = 0;
    int rc = wait4(child, &st, WUNTRACED, 0);
    if (rc != child || !WIFSTOPPED(st) || WSTOPSIG(st) != SIGSTOP) {
        printf("[sigtest] FAILED: stop rc=%d st=%d\n", rc, st);
        return 0;
    }
    printf("[sigtest] SIGSTOP + WIFSTOPPED passed\n");

    kill(child, SIGCONT);
    st = 0;
    rc = wait4(child, &st, 0, 0);
    if (rc != child || !WIFEXITED(st) || WEXITSTATUS(st) != 5) {
        printf("[sigtest] FAILED: continue rc=%d st=%d\n", rc, st);
        return 0;
    }
    printf("[sigtest] SIGCONT + WIFEXITED passed\n");
    return 1;
}

static int check_signalled_status(void) {
    int child = fork();
    if (child < 0) { printf("[sigtest] FAILED: fork\n"); return 0; }
    if (child == 0) { for (;;) { yield(); } }

    for (volatile int i = 0; i < 2000000; i++) { }
    kill(child, SIGKILL);

    int st = 0;
    int rc = wait4(child, &st, 0, 0);
    if (rc != child || !WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL) {
        printf("[sigtest] FAILED: kill rc=%d st=%d\n", rc, st);
        return 0;
    }
    printf("[sigtest] WIFSIGNALED passed\n");
    return 1;
}
```

- [ ] **Step 9: Commit**

```bash
git add kernel/sched/proc.h kernel/sched/proc.c kernel/signal.c \
        kernel/syscall.c kernel/errno.h lib/syscall.c lib/wait.c \
        lib/include/sys/wait.h lib/include/errno.h userland/sigtest.c Makefile
git commit -m "Add wait4 with POSIX status encoding, SIGCHLD, and process groups"
```

---

## Task 10: `rt_sigtimedwait`, `rt_sigqueueinfo`, and timed waits

**Files:**
- Modify: `kernel/waitq.h`, `kernel/waitq.c`, `kernel/timer.h`, `kernel/timer.c`, `kernel/signal.c`, `kernel/syscall.c`, `lib/signal.c`, `lib/include/signal.h`

**Interfaces:**
- Produces: `uint64_t timer_ticks(void)`; `int waitq_sleep_timeout(struct waitq *q, struct spinlock *release, uint64_t deadline_ticks)`; `SYS_RT_SIGTIMEDWAIT` (26), `SYS_RT_SIGQUEUEINFO` (27).

- [ ] **Step 1: Expose the tick counter**

`kernel/timer.c` keeps `tick_count` file-static. Add:

```c
uint64_t timer_ticks(void) { return tick_count; }
```

and to `timer.h`:

```c
#define TIMER_HZ 100
uint64_t timer_ticks(void);
```

- [ ] **Step 2: Add a timed sleep to `waitq`**

```c
// Like waitq_sleep, but also returns -ETIMEDOUT once timer_ticks()
// reaches `deadline`. nanosleep and futex(FUTEX_WAIT) with a timeout
// need exactly this, so it is paid for once here.
int waitq_sleep_timeout(struct waitq *q, struct spinlock *release,
                        uint64_t deadline);
```

Implementation: record the deadline on the thread, and have
`timer_handler` walk a list of sleeping-with-deadline threads once per
tick, waking any whose deadline has passed. Keep the list a simple
singly-linked list on `struct thread` — at NeoOS's process counts a
scan per tick is cheaper than a heap and much easier to get right.

Add `#define ETIMEDOUT 110` to both `errno.h` files.

- [ ] **Step 3: Implement the two syscalls**

`rt_sigtimedwait` blocks until one of `set` is pending, then consumes
it *without* running a handler. `rt_sigqueueinfo` is `kill` with a
caller-supplied `siginfo` and `si_code = SI_QUEUE`.

- [ ] **Step 4: Extend `sigtest.c` with the RT-queueing check**

The test that distinguishes standard from real-time signals, and the
only one that proves the queue is real rather than a second pending
bit:

```c
static volatile int rt_count;
static void rt_handler(int sig) { (void)sig; rt_count++; }

static int check_rt_queueing(void) {
    struct sigaction sa;
    sa.sa_handler = rt_handler; sa.sa_flags = 0; sa.sa_mask = 0;
    sigaction(SIGUSR1,  &sa, 0);
    sigaction(SIGRTMIN, &sa, 0);

    sigset_t block = 0, old = 0;
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &block, &old);

    // Standard signal: two sends while blocked collapse into one.
    rt_count = 0;
    raise(SIGUSR1);
    raise(SIGUSR1);
    sigprocmask(SIG_SETMASK, &old, 0);
    if (rt_count != 1) {
        printf("[sigtest] FAILED: SIGUSR1 delivered %d times, want 1\n", rt_count);
        return 0;
    }

    // Real-time signal: two sends while blocked deliver twice.
    sigprocmask(SIG_BLOCK, &block, &old);
    rt_count = 0;
    raise(SIGRTMIN);
    raise(SIGRTMIN);
    sigprocmask(SIG_SETMASK, &old, 0);
    if (rt_count != 2) {
        printf("[sigtest] FAILED: SIGRTMIN delivered %d times, want 2\n", rt_count);
        return 0;
    }

    printf("[sigtest] RT queueing vs standard collapsing passed\n");
    return 1;
}
```

- [ ] **Step 5: Build, verify, commit**

```bash
git add kernel/waitq.h kernel/waitq.c kernel/timer.h kernel/timer.c \
        kernel/signal.c kernel/syscall.c kernel/errno.h lib/signal.c \
        lib/include/signal.h lib/include/errno.h userland/sigtest.c
git commit -m "Add rt_sigtimedwait, rt_sigqueueinfo, and timed waitq sleeps"
```

---

## Task 11: Documentation

**Files:**
- Modify: `docs/stdlib.md`

Per `CLAUDE.md` this is binding, and it is a task rather than a step
because there is a lot of it.

- [ ] **Step 1: Add the `<signal.h>` section**

Document `sigaction`, `signal`, `raise`, `kill`, `tkill`,
`sigprocmask`, `sigpending`, `sigsuspend`, `sigaltstack`, the
`sigset_t` helpers, and the `SA_*` flags.

- [ ] **Step 2: Add the `<sys/wait.h>` section**

Document `wait4`, `waitpid`, and every `W*` macro.

- [ ] **Step 3: Record the divergences explicitly**

`CLAUDE.md` requires deviations from POSIX/Linux to be called out:

- `wait` remains NeoOS-native (one pid, bare exit code) **beside**
  POSIX `wait4`. Neither replaces the other.
- No core dumps: `WIFSIGNALED`'s `0x80` core bit is defined but never
  set.
- No `SA_NOCLDWAIT`, and setting `SIGCHLD` to `SIG_IGN` does not
  auto-reap.
- `SIGTTIN`/`SIGTTOU` and orphaned-process-group `SIGHUP` are accepted
  but never generated: NeoOS has no controlling terminal.
- `sigqueue` depth is a fixed pool; exhaustion returns `-EAGAIN`.

- [ ] **Step 4: Update the `<errno.h>` section**

`ECHILD` (10), `ENOMEM` (12), `ETIMEDOUT` (110).

- [ ] **Step 5: Commit**

```bash
git add docs/stdlib.md
git commit -m "Document the signal and wait APIs and their divergences"
```

---

## Task 12: Leak gate and final regression

**Files:**
- Modify: `kernel/kernel.c` (temporary, reverted)

- [ ] **Step 1: Add the temporary leak-gate thread**

As in the VFS and threads milestones, `wait4` needs a valid current
thread, so this runs as a kernel thread. Add above `kmain` in
`kernel/kernel.c`:

```c
static void signal_leak_test(void) {
    serial_write_string("[test] before: free_frames=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string(" vnodes=");
    serial_write_hex64(vfs_vnode_in_use_count());
    serial_write_string("\n");

    for (int i = 0; i < 5; i++) {
        struct process *p = spawn("/BIN/SIGTEST.ELF");
        if (!p) {
            serial_write_string("[test] spawn FAILED\n");
        } else {
            wait_for_pid(p->pid);
        }
    }

    serial_write_string("[test] after: free_frames=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string(" vnodes=");
    serial_write_hex64(vfs_vnode_in_use_count());
    serial_write_string("\n");

    thread_exit_self(0);
}
```

Temporarily replace `kmain`'s `spawn(...)` calls with
`thread_alloc_kernel(signal_leak_test);`.

- [ ] **Step 2: Run it, then run it again at 10 iterations**

**Do not accept a nonzero delta as one-time without checking.** The
threads milestone's gate showed 7 frames at 5 iterations, which looked
like a fixed cost and was in fact ~1 frame leaked per cycle. Only the
10-iteration comparison revealed it. The delta must be **identical** at
5 and at 10.

Signal-specific suspects if it grows: `sigqueue` pool entries not
returned by `signal_take_pending`, and threads that stop and are killed
while `THREAD_STOPPED` never reaching the zombie list.

- [ ] **Step 3: Revert and run the final regression**

Restore `kmain`'s spawns and confirm `git diff --stat kernel/kernel.c`
shows only the `SIGTEST`/`FAULTER` additions.

Checked against the spec's success criteria one by one:

- `[signal] selftest passed`, plus every prior selftest (`pmm`,
  `paging`, `lock`, `heap`, `fat16`, `fat16 write`, `vfs`, `waitq`).
- `[sigtest] ALL PASSED` with every check: handler + sigreturn,
  blocking + sigpending, `EINTR`, `SA_RESTART`, RT queueing versus
  standard collapsing, `SIGSEGV` on the altstack, stop/continue with
  `WIFSTOPPED`, and `WIFSIGNALED` on `SIGKILL`.
- **`faulter` dies alone**: its `[process] task exited` line appears and
  the other processes complete. **Zero `exception` lines.**
- `[threadtest] ALL PASSED`, `[vfstest] ALL PASSED`,
  `[parent] child exit code=42` — milestones 5-11 behavior unchanged.
- Zero `FAILED`.

- [ ] **Step 4: Commit (only if the gate caught something)**

Nothing to commit if the gate passed and `kernel.c` is reverted.

---

## Notes for the implementer

- **The frame layout and the trampoline are this milestone's
  `swapgs`.** In the threads milestone the conditional `swapgs` cost
  more debugging time than everything else combined, and the same shape
  of bug lives here: `struct iret_ctx`'s field order must match
  `sigframe.asm`'s pop sequence exactly, and nothing checks that but
  your eyes. A handler that runs but returns to garbage means the
  mismatch is in the tail of that struct.
- **Alignment is not optional.** The handler must see `rsp % 16 == 8`,
  exactly as if reached by `call`. Get this wrong and any handler using
  SSE — including anything musl compiles — faults on its first aligned
  spill, far from the actual bug.
- **`sigreturn` cannot use the `sysret` epilogue.** If you find yourself
  writing `frame->rcx = ...` in `signal_do_sigreturn`, stop: that path
  cannot restore a context interrupted by preemption.
- **Delivery is a loop, not an if.** Several signals can be pending at
  once, and each handler entry stacks another frame on the user stack.
- **Fault signals must be force-delivered** when blocked or ignored, or
  the process spins re-faulting forever.
- **Every verification needs a fresh disk image.** `fat16_write_selftest`
  creates `/NEWDIR`; a stale image reports `FAILED` on the second and
  later boots. This has caused false alarms twice already.
- **QEMU never exits on its own.** Always wrap it in `timeout`.
- `kernel/signal.c` will be around 700 lines. That is acceptable; if a
  natural seam appears (sending separating cleanly from delivery), note
  it for a follow-up rather than acting on it mid-milestone.
