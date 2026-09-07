// kernel/syscall/sys_sched.c -- the userland scheduler ABI (SCH-1 T5).
//
// These sit on top of the EEVDF fair class (kernel/sched/fair.c). The
// syscall NUMBERS are NeoOS's own (syscall_nr.h); the shim maps musl's
// Linux numbers onto them. Struct layouts and constant values crossing
// the boundary (sched_param, sched_attr, SCHED_*) are Linux-shaped --
// see docs/stdlib.md for the deliberate divergences.

#include "syscall/syscall_internal.h"
#include "sched/proc.h"
#include "sched/rq.h"
#include "sched/sched_entity.h"
#include "smp/smp.h"
#include "arch/cpu_local.h"
#include "mm/uaccess.h"
#include "errno.h"
#include "ipc/signal.h"   // struct k_timespec

// Linux x86_64 struct sched_attr (unistd sched_setattr(2)).
struct k_sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t  sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime;
    uint64_t sched_deadline;
    uint64_t sched_period;
};

// Resolve a sched-ABI pid/tid argument to a thread. pid 0, or the
// caller's own pid/tid, means "me". Any other value looks up the
// process and takes its first live thread (NeoOS has no per-tid sched
// state to distinguish siblings yet -- documented divergence).
static struct thread *sched_target(int64_t pid, int *err) {
    *err = 0;
    struct thread  *me = current_thread();
    struct process *mp = current_proc();
    if (pid == 0 || (mp && pid == mp->pid) || (me && pid == me->tid)) {
        return me;
    }
    struct process *p = proc_find((int)pid);
    if (!p) { *err = -ESRCH; return 0; }
    if (!p->threads) { *err = -ESRCH; return 0; }
    return p->threads;
}

static int policy_ok(int policy) {
    return policy == SCHED_NORMAL || policy == SCHED_BATCH || policy == SCHED_IDLE;
}

// ---- nice / priority ------------------------------------------------

#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

int64_t sys_setpriority(struct syscall_args *a) {
    int which = (int)a->a1;
    int who   = (int)a->a2;
    int prio  = (int)a->a3;
    if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER) {
        return -EINVAL;
    }
    if (which != PRIO_PROCESS) { return -EPERM; }   // no pgrp/user sets yet
    int err;
    struct thread *t = sched_target(who, &err);
    if (err) { return err; }
    if (prio < -20) { prio = -20; }
    if (prio >  19) { prio =  19; }
    // Lowering another process's nice (raising priority) needs privilege.
    struct process *mp = current_proc();
    if (t->proc && mp && t->proc != mp && mp->uid != 0 && prio < t->se.nice) {
        return -EACCES;
    }
    sched_apply_attr(t, prio, t->se.policy, (uint64_t)-1);
    return 0;
}

int64_t sys_getpriority(struct syscall_args *a) {
    int which = (int)a->a1;
    int who   = (int)a->a2;
    if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER) {
        return -EINVAL;
    }
    int err;
    struct thread *t = sched_target(who, &err);
    if (err) { return err; }
    // Linux returns 20 - nice so the syscall result stays non-negative;
    // musl's getpriority() wrapper subtracts it back.
    return 20 - t->se.nice;
}

int64_t sys_nice(struct syscall_args *a) {
    int inc = (int)a->a1;
    struct thread *t = current_thread();
    if (!t) { return -EPERM; }
    int nn = t->se.nice + inc;
    if (nn < -20) { nn = -20; }
    if (nn >  19) { nn =  19; }
    sched_apply_attr(t, nn, t->se.policy, (uint64_t)-1);
    return nn;
}

// ---- scheduler policy ---------------------------------------------------

int64_t sys_sched_getscheduler(struct syscall_args *a) {
    int err;
    struct thread *t = sched_target(a->a1, &err);
    if (err) { return err; }
    return t->se.policy;
}

int64_t sys_sched_setscheduler(struct syscall_args *a) {
    int err;
    struct thread *t = sched_target(a->a1, &err);
    if (err) { return err; }
    int policy = (int)a->a2;
    if (policy == SCHED_FIFO || policy == SCHED_RR || policy == SCHED_DEADLINE) {
        return -EINVAL;   // real-time classes: SCH-3 / SCH-4 (docs/stdlib.md)
    }
    if (!policy_ok(policy)) { return -EINVAL; }
    // struct sched_param { int sched_priority; } -- must be 0 for the
    // fair policies.
    if (a->a3) {
        int prio = 0;
        if (copy_from_user(&prio, (const void *)(uintptr_t)a->a3, sizeof(prio)) != 0) {
            return -EFAULT;
        }
        if (prio != 0) { return -EINVAL; }
    }
    sched_apply_attr(t, t->se.nice, policy, (uint64_t)-1);
    return 0;
}

int64_t sys_sched_getparam(struct syscall_args *a) {
    int err;
    struct thread *t = sched_target(a->a1, &err);
    if (err) { return err; }
    if (!a->a2) { return -EINVAL; }
    int prio = 0;   // fair policies have no RT priority
    if (copy_to_user((void *)(uintptr_t)a->a2, &prio, sizeof(prio)) != 0) {
        return -EFAULT;
    }
    return 0;
}

int64_t sys_sched_setparam(struct syscall_args *a) {
    int err;
    struct thread *t = sched_target(a->a1, &err);
    if (err) { return err; }
    (void)t;
    if (!a->a2) { return -EINVAL; }
    int prio = 0;
    if (copy_from_user(&prio, (const void *)(uintptr_t)a->a2, sizeof(prio)) != 0) {
        return -EFAULT;
    }
    return prio == 0 ? 0 : -EINVAL;
}

int64_t sys_sched_get_priority_max(struct syscall_args *a) {
    int policy = (int)a->a1;
    if (policy == SCHED_FIFO || policy == SCHED_RR) { return 99; }
    if (policy_ok(policy)) { return 0; }
    return -EINVAL;
}

int64_t sys_sched_get_priority_min(struct syscall_args *a) {
    int policy = (int)a->a1;
    if (policy == SCHED_FIFO || policy == SCHED_RR) { return 1; }
    if (policy_ok(policy)) { return 0; }
    return -EINVAL;
}

int64_t sys_sched_rr_get_interval(struct syscall_args *a) {
    if (!a->a2) { return -EINVAL; }
    struct k_timespec ts = {
        .tv_sec  = 0,
        .tv_nsec = (int64_t)SCHED_BASE_SLICE_NS,
    };
    if (copy_to_user((void *)(uintptr_t)a->a2, &ts, sizeof(ts)) != 0) {
        return -EFAULT;
    }
    return 0;
}

// ---- sched_setattr / sched_getattr ------------------------------------

int64_t sys_sched_setattr(struct syscall_args *a) {
    int err;
    struct thread *t = sched_target(a->a1, &err);
    if (err) { return err; }
    if (!a->a2) { return -EINVAL; }

    struct k_sched_attr at;
    for (unsigned i = 0; i < sizeof(at); i++) { ((uint8_t *)&at)[i] = 0; }
    // The struct is versioned by its leading size field; copy at most
    // what we know and tolerate a caller that passed a shorter one.
    uint32_t sz = 0;
    if (copy_from_user(&sz, (const void *)(uintptr_t)a->a2, sizeof(sz)) != 0) {
        return -EFAULT;
    }
    if (sz == 0) { sz = sizeof(at); }
    if (sz > sizeof(at)) { sz = sizeof(at); }
    if (copy_from_user(&at, (const void *)(uintptr_t)a->a2, sz) != 0) {
        return -EFAULT;
    }

    int policy = (int)at.sched_policy;
    if (policy == SCHED_FIFO || policy == SCHED_RR || policy == SCHED_DEADLINE) {
        return -EINVAL;
    }
    if (!policy_ok(policy)) { policy = t->se.policy; }

    int nice = at.sched_nice;
    if (nice < -20) { nice = -20; }
    if (nice >  19) { nice =  19; }

    uint64_t slice = (uint64_t)-1;
    if (at.sched_runtime) {
        slice = at.sched_runtime;
        uint64_t lo = SCHED_BASE_SLICE_NS / 16;
        uint64_t hi = SCHED_BASE_SLICE_NS * 100;
        if (slice < lo) { slice = lo; }
        if (slice > hi) { slice = hi; }
    }
    sched_apply_attr(t, nice, policy, slice);
    return 0;
}

int64_t sys_sched_getattr(struct syscall_args *a) {
    int err;
    struct thread *t = sched_target(a->a1, &err);
    if (err) { return err; }
    uint32_t usize = (uint32_t)a->a3;
    if (!a->a2 || usize < sizeof(struct k_sched_attr)) { return -EINVAL; }

    struct k_sched_attr at;
    for (unsigned i = 0; i < sizeof(at); i++) { ((uint8_t *)&at)[i] = 0; }
    at.size          = sizeof(at);
    at.sched_policy   = (uint32_t)t->se.policy;
    at.sched_nice     = t->se.nice;
    at.sched_priority = 0;
    at.sched_runtime  = t->se.slice ? t->se.slice : SCHED_BASE_SLICE_NS;

    if (copy_to_user((void *)(uintptr_t)a->a2, &at, sizeof(at)) != 0) {
        return -EFAULT;
    }
    return 0;
}

// ---- affinity ---------------------------------------------------------

// sched_setaffinity(pid, cpusetsize, mask). NeoOS records the mask on
// the thread (cpus_allowed); it is honoured at wake placement and by
// the work stealer. An empty intersection with the online set is
// -EINVAL, matching Linux.
int64_t sys_sched_setaffinity(struct syscall_args *a) {
    int err;
    struct thread *t = sched_target(a->a1, &err);
    if (err) { return err; }
    uint64_t cpusetsize = (uint64_t)a->a2;
    uint64_t mask_ptr   = (uint64_t)a->a3;
    if (!mask_ptr) { return -EFAULT; }
    if (cpusetsize == 0) { return -EINVAL; }
    if (cpusetsize > 8) { cpusetsize = 8; }   // NeoOS caps affinity at 64 CPUs

    uint64_t mask = 0;
    if (copy_from_user(&mask, (const void *)(uintptr_t)mask_ptr, cpusetsize) != 0) {
        return -EFAULT;
    }
    int online = smp_online_count();
    uint64_t online_mask = (online >= 64) ? ~0ULL : ((1ULL << online) - 1);
    if ((mask & online_mask) == 0) { return -EINVAL; }
    t->cpus_allowed = mask ? mask : ~0ULL;
    return 0;
}

// ---- yield ----------------------------------------------------------

int64_t sys_yield(struct syscall_args *a) {
    (void)a;
    sched_do_yield();
    return 0;
}
