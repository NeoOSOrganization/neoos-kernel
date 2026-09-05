// kernel/syscall/sys_signal.c -- Signal dispositions, masks, delivery and the sigreturn path.
//
// Split out of the former 997-line kernel/syscall.c. The handlers are
// unchanged; only the dispatch table, the MSR setup and the shared
// user-copy helpers stayed behind in syscall.c.

#include "syscall/syscall_internal.h"
#include "drivers/char/serial.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "errno.h"
#include "sync/lock.h"
#include "ipc/signal.h"
#include "ipc/futex.h"
#include "ipc/pipe.h"
#include "drivers/char/timer.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "arch/cpu_local.h"
#include "smp/smp.h"
#include "net/socket.h"

int64_t sys_rt_sigaction(struct syscall_args *a) {
    int sig = (int)a->a1;
    uint64_t act_ptr = (uint64_t)a->a2;
    uint64_t old_ptr = (uint64_t)a->a3;
    if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
    // SIGKILL and SIGSTOP can be neither caught nor ignored.
    if (act_ptr && (sig == SIGKILL || sig == SIGSTOP)) { return -EINVAL; }

    // Both user copies happen OUTSIDE the p->lock critical section:
    // p->lock is taken via spin_lock_irqsave (IRQs off), and a
    // copy_to_user/copy_from_user that faults can call into vma_fault,
    // which can allocate and sleep -- not safe with IRQs disabled and a
    // spinlock held. Read the new action first, swap it in under the
    // lock, then report the old one back to user memory afterward.
    struct k_sigaction new_act;
    if (act_ptr) {
        uint64_t missed = copy_from_user(&new_act, (const void *)(uintptr_t)act_ptr, sizeof new_act);
        if (missed > 0) { return -EFAULT; }
    }

    struct process *p = current_proc();
    uint64_t fl = spin_lock_irqsave(&p->lock);
    struct k_sigaction prev = p->actions[sig];
    if (act_ptr) {
        p->actions[sig] = new_act;
        p->actions[sig].mask &= ~SIGSET_UNBLOCKABLE;
    }
    spin_unlock_irqrestore(&p->lock, fl);

    if (old_ptr) {
        uint64_t missed = copy_to_user((void *)(uintptr_t)old_ptr, &prev, sizeof prev);
        if (missed > 0) { return -EFAULT; }
    }
    return 0;
}

int64_t sys_rt_sigprocmask(struct syscall_args *a) {
    int how = (int)a->a1;
    uint64_t set_ptr = (uint64_t)a->a2;
    uint64_t old_ptr = (uint64_t)a->a3;
    struct thread *t = current_thread();
    if (old_ptr) {
        uint64_t missed = copy_to_user((void *)(uintptr_t)old_ptr, &t->blocked, sizeof t->blocked);
        if (missed > 0) { return -EFAULT; }
    }
    if (set_ptr) {
        sigset_t_k v;
        uint64_t missed = copy_from_user(&v, (const void *)(uintptr_t)set_ptr, sizeof v);
        if (missed > 0) { return -EFAULT; }
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
    uint64_t out_ptr = (uint64_t)a->a1;
    struct thread *t = current_thread();
    if (out_ptr) {
        // Pending AND blocked: an unblocked pending signal would already
        // have been delivered.
        sigset_t_k pending = (t->pending | t->proc->pending) & t->blocked;
        uint64_t missed = copy_to_user((void *)(uintptr_t)out_ptr, &pending, sizeof pending);
        if (missed > 0) { return -EFAULT; }
    }
    return 0;
}

int64_t sys_rt_sigsuspend(struct syscall_args *a) {
    uint64_t mask_ptr = (uint64_t)a->a1;
    sigset_t_k mask;
    uint64_t missed = copy_from_user(&mask, (const void *)(uintptr_t)mask_ptr, sizeof mask);
    if (missed > 0) { return -EFAULT; }
    struct thread *t = current_thread();
    t->saved_blocked = t->blocked;
    t->in_sigsuspend = 1;
    t->blocked = mask & ~SIGSET_UNBLOCKABLE;
    while (signal_next_deliverable(t) == 0) {
        if (waitq_sleep(&t->proc->sig_waiters, 0) == -EINTR) { break; }
    }
    // The delivery path restores saved_blocked into the frame it
    // builds, so sigsuspend always reports interruption.
    return -EINTR;
}

int64_t sys_rt_sigtimedwait(struct syscall_args *a) {
    uint64_t set_ptr = (uint64_t)a->a1;
    uint64_t out_ptr  = (uint64_t)a->a2;
    uint64_t ts_ptr   = (uint64_t)a->a3;
    if (!set_ptr) { return -EINVAL; }
    sigset_t_k set;
    uint64_t missed = copy_from_user(&set, (const void *)(uintptr_t)set_ptr, sizeof set);
    if (missed > 0) { return -EFAULT; }
    struct k_timespec ts;
    int have_ts = 0;
    if (ts_ptr) {
        missed = copy_from_user(&ts, (const void *)(uintptr_t)ts_ptr, sizeof ts);
        if (missed > 0) { return -EFAULT; }
        have_ts = 1;
    }
    struct thread *t = current_thread();

    // Accept the wanted signals for the duration, so they become
    // pending rather than being delivered to a handler.
    sigset_t_k want = set & ~SIGSET_UNBLOCKABLE;
    sigset_t_k saved = t->blocked;
    t->blocked |= want;

    uint64_t deadline = 0;
    if (have_ts) {
        uint64_t ticks = (uint64_t)ts.tv_sec * TIMER_HZ
                       + (uint64_t)ts.tv_nsec / (1000000000UL / TIMER_HZ);
        deadline = timer_ticks() + (ticks ? ticks : 1);
    }

    int64_t rc = -EAGAIN;
    for (;;) {
        int sig = signal_next_pending_in(t, want);
        if (sig) {
            struct siginfo info;
            signal_take_pending(t, sig, &info);
            if (out_ptr) {
                uint64_t m = copy_to_user((void *)(uintptr_t)out_ptr, &info, sizeof info);
                if (m > 0) { rc = -EFAULT; break; }
            }
            rc = sig;
            break;
        }
        int wrc = have_ts ? waitq_sleep_timeout(&t->proc->sig_waiters, 0, deadline)
                          : waitq_sleep(&t->proc->sig_waiters, 0);
        if (wrc == -ETIMEDOUT) { rc = -EAGAIN; break; }
        if (wrc == -EINTR)     { rc = -EINTR;  break; }
    }
    t->blocked = saved;
    return rc;
}

int64_t sys_rt_sigqueueinfo(struct syscall_args *a) {
    int pid = (int)a->a1, sig = (int)a->a2;
    uint64_t user_ptr = (uint64_t)a->a3;
    struct siginfo info;
    siginfo_user(&info, sig, current_proc()->pid);
    if (user_ptr) {
        struct siginfo user;
        uint64_t missed = copy_from_user(&user, (const void *)(uintptr_t)user_ptr, sizeof user);
        if (missed > 0) { return -EFAULT; }
        info.si_code = SI_QUEUE;
        info.fields.rt.si_value = user.fields.rt.si_value;
    }
    return signal_kill(pid, sig, &info);
}

int64_t sys_rt_sigreturn(struct syscall_args *a) {
    signal_do_sigreturn(a->frame);
    return 0; // unreachable
}

int64_t sys_sigaltstack(struct syscall_args *a) {
    uint64_t ss_ptr = (uint64_t)a->a1;
    uint64_t old_ptr = (uint64_t)a->a2;
    struct thread *t = current_thread();
    if (old_ptr) {
        uint64_t missed = copy_to_user((void *)(uintptr_t)old_ptr, &t->altstack, sizeof t->altstack);
        if (missed > 0) { return -EFAULT; }
    }
    if (ss_ptr) {
        stack_t_k ss;
        uint64_t missed = copy_from_user(&ss, (const void *)(uintptr_t)ss_ptr, sizeof ss);
        if (missed > 0) { return -EFAULT; }
        if (ss.ss_size < 2048) { return -ENOMEM; }
        t->altstack = ss;
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
