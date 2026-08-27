# Signals Milestone

**Date:** 2026-08-27
**Status:** Approved
**Roadmap position:** Milestone 3 of 17, taken before milestone 2
(extended state) at the user's direction
(see `2026-08-27-roadmap-architecture-design.md`)

## Purpose

Give NeoOS POSIX signals, job control, and `wait4`.

This is a musl prerequisite and the reason it moved ahead of extended
state: musl needs `rt_sigaction`/`rt_sigprocmask`/`rt_sigreturn` for
`abort()` and `raise()`, and pthread cancellation delivers `SIGCANCEL`
(signal 32) to a specific thread — so real-time signal numbers and a
64-bit `sigset_t` are required even for a minimal port.

It is also the milestone that stops a user-mode fault from taking the
kernel down.

## What the current code forces

- **A ring-3 fault halts the whole machine.** `exception_dump_and_halt`
  (`kernel/isr.c:23`) does `cli; hlt` on any exception regardless of
  privilege level. `userland/faulter.c` exists and deliberately divides
  by zero; it is excluded from the standard boot because it would take
  the kernel with it.
- **`struct registers` has no `rsp`/`ss`** (`kernel/isr.h:9`). In long
  mode the CPU always pushes them, so the values are on the stack — the
  struct simply stops at `rflags`. Delivering a signal from a fault
  needs the user RSP to build a frame on.
- **The delivery hook already exists.** The threads milestone gave
  `syscall_dispatch` a single exit point so an unwinding thread could
  not return to user mode; signal delivery belongs there.
- **`kill_pending` is a one-signal prototype of this.** So is
  `waitq_sleep`'s `-EINTR`. Both fold into the general mechanism rather
  than sitting beside it.

## Decisions

| # | Decision | Chosen |
|---|---|---|
| 1 | Scope | Fuller POSIX, not the musl minimum |
| 2 | Job control | Included: `THREAD_STOPPED`, `SIGSTOP`/`SIGCONT`, `wait4` |
| 3 | Numbering and layout | Linux x86-64, verbatim |
| 4 | `SA_RESTORER` | Mandatory; the kernel never injects a trampoline |
| 5 | Delivery points | Syscall return **and** interrupt return to ring 3 |
| 6 | Signal return | `iretq` trampoline, not the `sysret` epilogue |

Decision 3 is forced by the musl direction: musl's own `signal.h` and
its `SA_SIGINFO` handlers read `ucontext_t` directly, so diverging
would mean patching musl's headers — emulation creeping into what is
supposed to be a translation shim.

Decision 1's cost, recorded honestly: most of what "fuller POSIX" adds
beyond the musl minimum (`sigqueue` payloads, process groups,
`sigtimedwait`) has no consumer in NeoOS yet, and every addition
threads through the same delivery path. This is expected to be the
largest milestone since the VFS.

## Data model

```c
#define NSIG      65          /* signals 1..64; index 0 unused */
#define SIGRTMIN  32          /* musl: 32 = SIGCANCEL, 33 = SIGSYNCCALL */

typedef uint64_t sigset_t_k;  /* one bit per signal */

/* Matches Linux's x86-64 rt_sigaction argument exactly. */
struct k_sigaction {
    void        (*handler)(int);
    unsigned long flags;
    void        (*restorer)(void);
    sigset_t_k    mask;       /* blocked *during* the handler */
};
```

Dispositions are per-process, masks are per-thread, pending sets exist
in both — the POSIX model, and the one musl expects.

```c
struct process {
    ...
    struct k_sigaction actions[NSIG];   /* shared by all threads */
    struct spinlock    sig_lock;        /* rank between PROCESS and THREAD */
    sigset_t_k         pending;         /* process-directed: kill() */
    struct sigqueue   *queued;          /* RT payloads, process-directed */
    int                pgid, sid;
    int                exit_signal;     /* 0, or the signal that killed it */
    int                stopped_count;   /* threads parked in THREAD_STOPPED */
};

struct thread {
    ...
    sigset_t_k       blocked;
    sigset_t_k       pending;           /* thread-directed: tkill() */
    struct sigqueue *queued;
    stack_t          altstack;
    sigset_t_k       saved_blocked;     /* for sigsuspend / sigreturn */
    int              in_sigsuspend;
};
```

**Standard signals (1–31) are not queued** — one pending bit each,
repeat deliveries collapse. **RT signals (32–64) queue** with their
`siginfo` payloads, FIFO per signal number, from a fixed-size pool.
Pool exhaustion fails the send with `-EAGAIN`, which POSIX permits and
which keeps this bounded in a kernel with no swap.

**`SIGKILL` and `SIGSTOP` can be neither caught nor blocked.**
`rt_sigaction` rejects them with `-EINVAL`, and `rt_sigprocmask`
silently drops them from any mask it is given, as POSIX requires.

**Selection on delivery:** lowest-numbered unblocked pending signal,
thread-directed before process-directed. A process-directed signal
goes to any one thread that does not block it.

### Two existing mechanisms fold in

- **`kill_pending` becomes `SIGKILL`.** The threads milestone's
  exit-kills-siblings was a one-signal prototype; keeping both would
  mean two ways to terminate a thread and two checks on the syscall
  path.
- **`waitq_sleep`'s `-EINTR` becomes the general interruption path**,
  driven by "a deliverable signal is pending" rather than a single
  flag. `SA_RESTART` then decides restart versus `-EINTR`.

## Delivery

### Frame layout

Linux's `rt_sigframe`, verbatim:

```
user stack, growing down, 16-byte aligned so the handler sees
rsp == 8 (mod 16) -- exactly as if reached by `call`:

    struct rt_sigframe {
        void            *pretcode;   /* -> sa_restorer */
        struct ucontext  uc;         /* mcontext: GP regs, RIP, RFLAGS, RSP */
        struct siginfo   info;
    };
    /* FP state follows uc, pointed to by uc.uc_mcontext.fpstate */
```

**Consequence of taking signals before extended state:** the frame's
FP area is FXSAVE-shaped (512 bytes) here, and the XSAVE milestone must
widen it and add the `xstate` header. That is the cost of the reorder —
one structure defined twice.

### Register frame

`struct registers` gains `rsp` and `ss` after `rflags`. The values are
already on the stack; only the declaration is missing.

### Delivery points

`signal_deliver_pending()` is called from two places, and takes the
user register state from whichever struct the caller has:

1. the `syscall_dispatch` exit point, and
2. `isr_handler`, on return to ring 3.

Without the second, a signal cannot interrupt a compute loop.

### Signal return

`rt_sigreturn` builds an `iretq` frame on the kernel stack and returns
through a small assembly trampoline, as Linux does. It cannot use the
ordinary `sysret` epilogue: `sysretq` takes RIP from `RCX` and RFLAGS
from `R11` and refuses a non-canonical RIP, so it cannot faithfully
restore an arbitrary interrupted context — which is exactly what
`sigreturn` must do when the signal arrived during preemption rather
than during a syscall.

### Syscall restart

`struct syscall_frame` has no `rax` field: the number arrives in `RAX`
and the return value leaves in it. `SA_RESTART` therefore rewinds
`frame->rcx` by 2 (the length of `syscall`) and **returns the syscall
number from `syscall_dispatch`**, which lands in `RAX` as the restarted
call's number. Without `SA_RESTART` the call returns `-EINTR`.

### sigaltstack

Earns its place immediately. The threads milestone gave every thread a
guard page below its stack, so a stack overflow now raises `SIGSEGV` —
and a handler without an alternate stack would re-fault on the same
guard page. `SA_ONSTACK` is what makes such a handler runnable.

### Nesting

During a handler, `sa_mask | {signum}` is blocked unless `SA_NODEFER`.
`sigreturn` restores the previous mask from `uc.uc_sigmask`, which is
also how `sigsuspend` unwinds.

### Faults to signals

For ring-3 faults only:

| Vector | Signal | `si_code` |
|---|---|---|
| 0 Divide Error | `SIGFPE` | `FPE_INTDIV` |
| 6 Invalid Opcode | `SIGILL` | `ILL_ILLOPC` |
| 13 GP Fault | `SIGSEGV` | `SI_KERNEL` |
| 14 Page Fault | `SIGSEGV`/`SIGBUS` | `SEGV_MAPERR`/`SEGV_ACCERR` |
| 16/19 x87/SIMD | `SIGFPE` | `FPE_FLTINV` |

A fault from ring 0 keeps `exception_dump_and_halt`: a kernel bug
should still stop the machine loudly. Only the ring-3 path changes.

## Job control

**`THREAD_STOPPED` joins the scheduler's state set.** `schedule()`
never picks a stopped thread. The idle thread from the threads
milestone is what makes this safe: a process whose every thread stops
no longer risks leaving the CPU with nothing runnable.

**Stop is process-wide.** `SIGSTOP` marks every thread; threads blocked
in an interruptible sleep are *woken* to reach a delivery point, as for
a kill, but park in `THREAD_STOPPED` instead of running a handler.
`SIGCONT` makes them `READY`; a thread that was mid-syscall then
restarts (`SA_RESTART`) or returns `-EINTR`.

The process counts as stopped once **all** threads have parked, tracked
by `stopped_count`. That is what `wait4(WUNTRACED)` reports, and it
avoids reporting a half-complete group stop.

**Process groups and sessions** live on `struct process` as
`pgid`/`sid`, inherited by `fork` and `spawn`, with
`setpgid`/`getpgid`/`setsid`/`getsid`. They exist because `kill` needs
them:

```
kill(pid > 0)    that process
kill(pid == 0)   every process in the caller's group
kill(pid < -1)   every process in group -pid
kill(pid == -1)  every process the caller may signal
```

**Termination status records how**: `exit_signal` sits alongside
`exit_code`, since `WIFEXITED` and `WIFSIGNALED` are different outcomes
that a bare exit code cannot distinguish.

## `wait` splits in two

The hybrid musl decision applied literally: keep the native call, add
the POSIX-shaped one.

```c
int64_t wait4(int pid, int *status, int options, void *rusage /* ignored */);

#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

/* status encoding, Linux-compatible */
exited     (code & 0xff) << 8
signaled   sig & 0x7f            (| 0x80 if core)
stopped    0x7f | (sig << 8)
continued  0xffff
```

`SYS_WAIT` (5) **keeps its current NeoOS-native signature** — one pid,
bare exit code — implemented as a thin wrapper over `wait4` that
decodes the status. Nothing in `userland/` breaks, `parent.c` keeps
working unchanged, and `docs/stdlib.md` already documents `wait` as
NeoOS-specific. musl gets `wait4`; NeoOS keeps `wait`.

**`SIGCHLD` is sent to the parent on child exit**, default action
ignore, which is what makes `wait4(WNOHANG)` polling loops behave.

## Syscalls

```
21 rt_sigaction      26 rt_sigtimedwait   31 tgkill
22 rt_sigprocmask    27 rt_sigqueueinfo   32 wait4
23 rt_sigreturn      28 sigaltstack       33 setpgid / 34 getpgid
24 rt_sigpending     29 kill              35 setsid  / 36 getsid
25 rt_sigsuspend     30 tkill
```

`gettid` is not among them: `SYS_THREAD_SELF` (20) already is `gettid`,
and the musl shim aliases it.

**`rt_sigtimedwait` pulls in a timed sleep.** `waitq` has none, so it
gains `waitq_sleep_timeout()`, converting a `timespec` against the
LAPIC tick rate `timer.c` already calibrates. `nanosleep` and
`futex(FUTEX_WAIT)` with a timeout need the same primitive, so it is
paid for once here.

## Standard library

Per `CLAUDE.md`, still pre-musl, so these are real `lib/` wrappers:

- `lib/include/signal.h` — `sigaction`, `sigprocmask`, `kill`, `raise`,
  `sigaltstack`, `sigsuspend`, and the `sigset_t` helpers.
- `lib/signal.c` — wrappers plus **the restorer**, since `SA_RESTORER`
  is mandatory and there is no musl yet to supply one:

```
__restore_rt:  mov $23, %rax    /* SYS_RT_SIGRETURN */
               syscall
```

  Every `sigaction()` fills it in automatically, so callers never see
  it.
- `lib/include/sys/wait.h` — `wait4`, `waitpid`, and the `W*` macros.
- `docs/stdlib.md` gains both sections, and notes the divergence:
  `wait` stays NeoOS-native beside POSIX `wait4`.

## Verification

Existing convention: in-kernel selftests announcing `passed`/`FAILED`,
a userland test program, headless QEMU under `timeout`, serial log
grepping. `-cpu Nehalem` still suffices.

In-kernel selftests cover what is testable without user mode:
`sigset_t` manipulation, and the pending/blocked selection order.

`userland/sigtest.c` proves the rest:

- a handler runs on `raise()`, and `sigreturn` restores the interrupted
  context intact
- blocking a signal makes `sigpending` report it; unblocking delivers
- `SA_RESTART` restarts a blocking call; without it it returns `-EINTR`
- **two `SIGRTMIN` sends while blocked deliver twice** (queued), while
  two `SIGUSR1` sends deliver once (collapsed)
- a null dereference raises `SIGSEGV` and the handler runs
- **a stack overflow raises `SIGSEGV` onto the `sigaltstack`** — the
  case that re-faults without one, and the reason the threads
  milestone's guard pages matter
- a child stopped with `SIGSTOP`, reported by `wait4(WUNTRACED)`,
  resumed with `SIGCONT`, exit reported with `WIFEXITED`
- a child killed by `SIGKILL` reports `WIFSIGNALED` with the right
  signal number

And the headline, needing no new program: **`userland/faulter.c`
rejoins the standard boot.** It divides by zero, which today `cli;
hlt`s the entire kernel. Afterwards it dies of `SIGFPE` alone while the
other five processes carry on. That one line in the boot log is the
milestone's clearest proof.

## Out of scope

Recorded so it is not relitigated:

- Orphaned-process-group `SIGHUP`.
- `SIGTTIN`/`SIGTTOU` terminal access control.
- `ptrace`-style stop reporting.

All three need a controlling terminal, which NeoOS does not have.

- Core dumps. `WIFSIGNALED`'s `0x80` core bit is defined but never set.
- `SA_NOCLDWAIT` and `SIGCHLD` set to `SIG_IGN` auto-reaping.
