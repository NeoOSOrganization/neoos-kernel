// kernel/syscall/sys_signal.c -- Signal dispositions, masks, delivery and the sigreturn path.
//
// Split out of the former 997-line kernel/syscall.c. The handlers are
// unchanged; only the dispatch table, the MSR setup and the shared
// user-copy helpers stayed behind in syscall.c.

#include "syscall/syscall_internal.h"
#include "dev/serial.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "errno.h"
#include "sync/lock.h"
#include "ipc/signal.h"
#include "ipc/futex.h"
#include "ipc/pipe.h"
#include "dev/timer.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "arch/cpu_local.h"
#include "smp/smp.h"
#include "net/socket.h"

int64_t sys_rt_sigaction(struct syscall_args *a) {
    int sig = (int)a->a1;
    const struct k_sigaction *act = (const struct k_sigaction *)(uintptr_t)a->a2;
    struct k_sigaction *old = (struct k_sigaction *)(uintptr_t)a->a3;
    if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
    // SIGKILL and SIGSTOP can be neither caught nor ignored.
    if (act && (sig == SIGKILL || sig == SIGSTOP)) { return -EINVAL; }

    struct process *p = current_proc();
    uint64_t fl = spin_lock_irqsave(&p->lock);
    if (old) { *old = p->actions[sig]; }
    if (act) {
        p->actions[sig] = *act;
        p->actions[sig].mask &= ~SIGSET_UNBLOCKABLE;
    }
    spin_unlock_irqrestore(&p->lock, fl);
    return 0;
}

int64_t sys_rt_sigprocmask(struct syscall_args *a) {
    int how = (int)a->a1;
    const sigset_t_k *set = (const sigset_t_k *)(uintptr_t)a->a2;
    sigset_t_k *old = (sigset_t_k *)(uintptr_t)a->a3;
    struct thread *t = current_thread();
    if (old) { *old = t->blocked; }
    if (set) {
        sigset_t_k v = *set;
        if (how == SIG_BLOCK)        { t->blocked |= v;  }
        else if (how == SIG_UNBLOCK) { t->blocked &= ~v; }
        else if (how == SIG_SETMASK) { t->blocked = v;   }
        else                         { return -EINVAL;   }
        // SIGKILL and SIGSTOP are silently dropped from any mask, as
        // POSIX requires -- not an error.
        t->blocked &= ~SIGSET_UNBLOCKABLE;
    }
    return 0;
}

int64_t sys_rt_sigpending(struct syscall_args *a) {
    sigset_t_k *out = (sigset_t_k *)(uintptr_t)a->a1;
    struct thread *t = current_thread();
    // Pending AND blocked: an unblocked pending signal would already
    // have been delivered.
    if (out) { *out = (t->pending | t->proc->pending) & t->blocked; }
    return 0;
}

int64_t sys_rt_sigsuspend(struct syscall_args *a) {
    const sigset_t_k *mask = (const sigset_t_k *)(uintptr_t)a->a1;
    struct thread *t = current_thread();
    t->saved_blocked = t->blocked;
    t->in_sigsuspend = 1;
    t->blocked = (*mask) & ~SIGSET_UNBLOCKABLE;
    while (signal_next_deliverable(t) == 0) {
        if (waitq_sleep(&t->proc->sig_waiters, 0) == -EINTR) { break; }
    }
    // The delivery path restores saved_blocked into the frame it
    // builds, so sigsuspend always reports interruption.
    return -EINTR;
}

int64_t sys_rt_sigtimedwait(struct syscall_args *a) {
    const sigset_t_k *set = (const sigset_t_k *)(uintptr_t)a->a1;
    struct siginfo *out   = (struct siginfo *)(uintptr_t)a->a2;
    const struct k_timespec *ts = (const struct k_timespec *)(uintptr_t)a->a3;
    if (!set) { return -EINVAL; }
    struct thread *t = current_thread();

    // Accept the wanted signals for the duration, so they become
    // pending rather than being delivered to a handler.
    sigset_t_k want = (*set) & ~SIGSET_UNBLOCKABLE;
    sigset_t_k saved = t->blocked;
    t->blocked |= want;

    uint64_t deadline = 0;
    if (ts) {
        uint64_t ticks = (uint64_t)ts->tv_sec * TIMER_HZ
                       + (uint64_t)ts->tv_nsec / (1000000000UL / TIMER_HZ);
        deadline = timer_ticks() + (ticks ? ticks : 1);
    }

    int64_t rc = -EAGAIN;
    for (;;) {
        int sig = signal_next_pending_in(t, want);
        if (sig) {
            struct siginfo info;
            signal_take_pending(t, sig, &info);
            if (out) { *out = info; }
            rc = sig;
            break;
        }
        int wrc = ts ? waitq_sleep_timeout(&t->proc->sig_waiters, 0, deadline)
                     : waitq_sleep(&t->proc->sig_waiters, 0);
        if (wrc == -ETIMEDOUT) { rc = -EAGAIN; break; }
        if (wrc == -EINTR)     { rc = -EINTR;  break; }
    }
    t->blocked = saved;
    return rc;
}

int64_t sys_rt_sigqueueinfo(struct syscall_args *a) {
    int pid = (int)a->a1, sig = (int)a->a2;
    const struct siginfo *user = (const struct siginfo *)(uintptr_t)a->a3;
    struct siginfo info;
    siginfo_user(&info, sig, current_proc()->pid);
    if (user) {
        info.si_code = SI_QUEUE;
        info.fields.rt.si_value = user->fields.rt.si_value;
    }
    return signal_kill(pid, sig, &info);
}

int64_t sys_rt_sigreturn(struct syscall_args *a) {
    signal_do_sigreturn(a->frame);
    return 0; // unreachable
}

int64_t sys_sigaltstack(struct syscall_args *a) {
    const stack_t_k *ss = (const stack_t_k *)(uintptr_t)a->a1;
    stack_t_k *old = (stack_t_k *)(uintptr_t)a->a2;
    struct thread *t = current_thread();
    if (old) { *old = t->altstack; }
    if (ss) {
        if (ss->ss_size < 2048) { return -ENOMEM; }
        t->altstack = *ss;
    }
    return 0;
}

int64_t sys_kill(struct syscall_args *a) {
    struct siginfo info;
    siginfo_user(&info, (int)a->a2, current_proc()->pid);
    return signal_kill((int)a->a1, (int)a->a2, &info);
}

int64_t sys_tkill(struct syscall_args *a) {
    struct siginfo info;
    siginfo_user(&info, (int)a->a2, current_proc()->pid);
    return signal_tkill(0, (int)a->a1, (int)a->a2, &info);
}

int64_t sys_tgkill(struct syscall_args *a) {
    struct siginfo info;
    siginfo_user(&info, (int)a->a3, current_proc()->pid);
    return signal_tkill((int)a->a1, (int)a->a2, (int)a->a3, &info);
}
