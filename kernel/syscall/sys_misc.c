// kernel/syscall/sys_misc.c -- SMP visibility and futex.
//
// Split out of the former 997-line kernel/syscall.c. The handlers are
// unchanged; only the dispatch table, the MSR setup and the shared
// user-copy helpers stayed behind in syscall.c.

#include "syscall/syscall_internal.h"
#include "ipc/memfd.h"
#include "net/tcp.h"
#include "mm/pmm.h"
#include "sync/waitq.h"
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
#include "drivers/char/rtc.h"
#include "time/ktime.h"
#include "drivers/input/input.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "arch/cpu_local.h"
#include "smp/smp.h"
#include "net/socket.h"
#include "mm/uaccess.h"

int64_t sys_cpu_count(struct syscall_args *a) {
    (void)a;
    return smp_online_count();
}

int64_t sys_getcpu(struct syscall_args *a) {
    (void)a;
    return (int)(this_cpu() - &cpus[0]);
}

int64_t sys_futex(struct syscall_args *a) {
    // Linux's argument order, unchanged: uaddr, op, val, timeout/val2,
    // uaddr2, val3. The fourth slot is a timeout POINTER for the WAIT
    // operations and an integer COUNT (val2) for the REQUEUE ones, so
    // both readings are passed down and futex_op picks by command.
    // uaddr2 is not read: NeoOS's requeue wakes rather than moves, so
    // there is no target queue to name (see futex.c).
    return futex_op((uint32_t *)(uintptr_t)a->a1, (int)a->a2, (uint32_t)a->a3,
                    (const struct k_timespec *)(uintptr_t)a->a4,
                    (uint32_t)a->a4, (uint32_t)a->frame->r9);
}

// ---- the clock -------------------------------------------------------
//
// Every clock reads ktime (kernel/time/ktime.c): nanoseconds since boot
// from the calibrated TSC. CLOCK_REALTIME is wall time, anchored to the
// CMOS RTC read once at boot (dev/rtc.c); CLOCK_MONOTONIC and friends
// count from boot; the CPU-time clocks are the scheduler's nanosecond
// runtime accounting.
//
// DIVERGENCE, recorded in docs/stdlib.md: if the RTC could not be read
// at boot CLOCK_REALTIME silently falls back to a boot epoch and
// formats as January 1970.

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_BOOTTIME           7

#define TICK_HZ     100
#define NS_PER_TICK (1000000000ULL / TICK_HZ)

static int clock_valid(int clk) {
    return clk == CLOCK_REALTIME || clk == CLOCK_MONOTONIC || clk == CLOCK_MONOTONIC_RAW ||
           clk == CLOCK_BOOTTIME || clk == CLOCK_PROCESS_CPUTIME_ID || clk == CLOCK_THREAD_CPUTIME_ID;
}

// ns of CPU this thread has had, including the stretch it is running now.
static uint64_t thread_cpu_ns(struct thread *t) {
    uint64_t ns = t->se.sum_exec_runtime;
    if (t == current_thread() && t->se.exec_start && ktime_get_ns() > t->se.exec_start) {
        ns += ktime_get_ns() - t->se.exec_start;
    }
    return ns;
}

static uint64_t process_cpu_ns(struct process *p) {
    uint64_t f = spin_lock_irqsave(&p->lock);
    uint64_t ns = p->cpu_ns_exited;
    for (struct thread *t = p->threads; t; t = t->proc_next) { ns += thread_cpu_ns(t); }
    spin_unlock_irqrestore(&p->lock, f);
    return ns;
}

static uint64_t clock_now_ns(int clk) {
    switch (clk) {
    case CLOCK_REALTIME:           return (uint64_t)rtc_boot_epoch() * NSEC_PER_SEC + ktime_get_ns();
    case CLOCK_PROCESS_CPUTIME_ID: return process_cpu_ns(current_proc());
    case CLOCK_THREAD_CPUTIME_ID:  return thread_cpu_ns(current_thread());
    default:                       return ktime_get_ns();
    }
}

int64_t sys_clock_gettime(struct syscall_args *a) {
    int clk = (int)a->a1;
    uint64_t out = (uint64_t)a->a2;
    if (!clock_valid(clk)) { return -EINVAL; }
    if (!out) { return -EFAULT; }
    uint64_t ns = clock_now_ns(clk);
    struct k_timespec ts = { .tv_sec = (int64_t)(ns / NSEC_PER_SEC), .tv_nsec = (int64_t)(ns % NSEC_PER_SEC) };
    if (copy_to_user((void *)(uintptr_t)out, &ts, sizeof ts) > 0) { return -EFAULT; }
    return 0;
}

// clock_getres(clockid, res) -- every clock is TSC-backed: 1 ns, as
// Linux reports for hrtimer clocks. res may be NULL (Linux allows it).
int64_t sys_clock_getres(struct syscall_args *a) {
    int clk = (int)a->a1;
    uint64_t out = (uint64_t)a->a2;
    if (!clock_valid(clk)) { return -EINVAL; }
    if (!out) { return 0; }
    struct k_timespec ts = { .tv_sec = 0, .tv_nsec = 1 };
    if (copy_to_user((void *)(uintptr_t)out, &ts, sizeof ts) > 0) { return -EFAULT; }
    return 0;
}

// Relative sleep, rounded UP to a whole tick: sleeping less than asked
// is a bug a caller cannot defend against, whereas sleeping slightly
// longer is what every tick-driven kernel does.
//
// DIVERGES: the remaining-time argument is ignored, because nothing
// here can interrupt a sleep partway and report a remainder yet.
// clock_nanosleep(clockid, flags, request, remain) -- MSC-2. The
// absolute-deadline sleep .NET / Go / musl's pthread_cond_timedwait
// want. TIMER_ABSTIME (flag 1) treats `request` as an absolute time on
// `clockid`; flags 0 is relative, identical to nanosleep. `remain` is
// ignored on -EINTR, the same divergence nanosleep documents.
#define TIMER_ABSTIME 1
int64_t sys_clock_nanosleep(struct syscall_args *a) {
    int      clk   = (int)a->a1;
    int      flags = (int)a->a2;
    uint64_t req_p = (uint64_t)a->a3;
    if (!req_p) { return -EFAULT; }
    if (clk != CLOCK_REALTIME && clk != CLOCK_MONOTONIC &&
        clk != CLOCK_MONOTONIC_RAW) {
        return -EINVAL;
    }

    struct k_timespec req;
    if (copy_from_user(&req, (const void *)(uintptr_t)req_p, sizeof req) != 0) {
        return -EFAULT;
    }
    if (req.tv_nsec < 0 || req.tv_nsec >= 1000000000L || req.tv_sec < 0) {
        return -EINVAL;
    }

    uint64_t deadline_ticks;
    if (flags & TIMER_ABSTIME) {
        int64_t sec = req.tv_sec;
        if (clk == CLOCK_REALTIME) { sec -= rtc_boot_epoch(); }
        if (sec < 0) { return 0; }   // deadline already in the past
        uint64_t ns = (uint64_t)sec * 1000000000ULL + (uint64_t)req.tv_nsec;
        deadline_ticks = (ns + NS_PER_TICK - 1) / NS_PER_TICK;
        if (deadline_ticks <= timer_ticks()) { return 0; }
    } else {
        uint64_t ns = (uint64_t)req.tv_sec * 1000000000ULL + (uint64_t)req.tv_nsec;
        uint64_t ticks = (ns + NS_PER_TICK - 1) / NS_PER_TICK;
        if (ticks == 0) { return 0; }
        deadline_ticks = timer_ticks() + ticks;
    }

    struct waitq q;
    waitq_init(&q);
    int rc = waitq_sleep_timeout(&q, NULL, deadline_ticks);
    return (rc == -EINTR) ? -EINTR : 0;
}

int64_t sys_nanosleep(struct syscall_args *a) {
    uint64_t uptr = (uint64_t)a->a1;
    if (!uptr) { return -EFAULT; }
    struct k_timespec req;
    uint64_t missed = copy_from_user(&req, (const void *)(uintptr_t)uptr, sizeof req);
    if (missed > 0) { return -EFAULT; }
    if (req.tv_nsec < 0 || req.tv_nsec >= 1000000000L || req.tv_sec < 0) {
        return -EINVAL;
    }

    uint64_t ns    = (uint64_t)req.tv_sec * 1000000000ULL + (uint64_t)req.tv_nsec;
    uint64_t ticks = (ns + NS_PER_TICK - 1) / NS_PER_TICK;
    if (ticks == 0 && ns > 0) { ticks = 1; }
    if (ticks == 0) { return 0; }

    uint64_t deadline = timer_ticks() + ticks;
    struct waitq q;
    waitq_init(&q);
    // Nothing ever wakes this queue; waitq_timeout_tick() dequeues the
    // sleeper when timer_ticks() reaches `deadline`. -EINTR if the
    // thread is killed while blocked, matching the interrupted-sleep
    // contract (rem is still ignored — documented).
    int rc = waitq_sleep_timeout(&q, NULL, deadline);
    if (rc == -EINTR) { return -EINTR; }
    return 0;
}

// Always present so the dispatch table has a real handler at
// SYS_TEST_HOOK (the table selftest asserts every slot up to SYS_MAX is
// filled). In a production build -- no -DNEOOS_TEST_HOOKS -- it just
// reports -ENOSYS, exactly as an unimplemented number would.
int64_t sys_test_hook(struct syscall_args *a) {
#ifdef NEOOS_TEST_HOOKS
    switch ((int)a->a1) {
    case TESTHOOK_INJECT_KEY:
        input_inject_key((uint16_t)a->a2, (int)a->a3);
        return 0;
    case TESTHOOK_MIG_COUNT:
        return (int64_t)smp_user_migration_count();
    case TESTHOOK_PMM_FREE:
        return (int64_t)pmm_free_frame_count();
    case TESTHOOK_POLL_DEPTH:
        return (int64_t)waitq_poll_depth();
    case TESTHOOK_POLL_WASTED:
        return (int64_t)waitq_poll_wasted();
    case TESTHOOK_POLL_STATS: {
        uint64_t ev = 0, wk = 0;
        waitq_poll_stats(&ev, &wk);
        // Both fit in 32 bits at any run length this suite reaches.
        return (int64_t)((ev << 32) | (wk & 0xFFFFFFFFu));
    }
    case TESTHOOK_TCP_FAULT:
        tcp_fault_inject((uint32_t)a->a2, (uint32_t)a->a3);
        return 0;
    case TESTHOOK_MEMFD_LIVE:
        return (int64_t)memfd_live_count();
    case TESTHOOK_TCP_RETRANS: {
        uint64_t rt = 0;
        tcp_stats(0, 0, &rt, 0, 0, 0);
        return (int64_t)rt;
    }
    case TESTHOOK_TCP_REASM: {
        uint64_t re = 0;
        tcp_stats(0, 0, 0, &re, 0, 0);
        return (int64_t)re;
    }
    case TESTHOOK_TCP_INUSE:
        return (int64_t)tcp_inuse();
    case TESTHOOK_PARENT_PID: {
        struct process *p = proc_find((int)a->a2);
        if (!p) { return -ESRCH; }
        int pp = p->parent_pid;
        proc_put(p);
        return pp;
    }
    default:
        return -EINVAL;
    }
#else
    (void)a;
    return -ENOSYS;
#endif
}

// sysinfo(info) -- Linux shape, matching musl's struct sysinfo
// (include/sys/sysinfo.h) field-for-field: uptime, loads[3], totalram,
// freeram, sharedram, bufferram, totalswap, freeswap, procs+pad,
// totalhigh, freehigh, mem_unit, then a 256-byte reserved tail. Real
// numbers where NeoOS has them (totalram/freeram, from pmm's frame
// counters), zero everywhere else -- no load average, no swap, no
// high memory, no uptime clock wired to this yet. mem_unit is 1 so
// the ram fields are read as exact bytes, not scaled.
struct neoos_sysinfo {
    uint64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs, pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    uint8_t  reserved[256];
};

int64_t sys_sysinfo(struct syscall_args *a) {
    uint64_t out = a->a1;
    if (!out) { return -EFAULT; }

    struct neoos_sysinfo si;
    for (unsigned i = 0; i < sizeof(si); i++) { ((uint8_t *)&si)[i] = 0; }
    si.totalram = pmm_total_frame_count() * PMM_FRAME_SIZE;
    si.freeram  = pmm_free_frame_count()  * PMM_FRAME_SIZE;
    si.mem_unit = 1;

    uint64_t missed = copy_to_user((void *)(uintptr_t)out, &si, sizeof si);
    if (missed > 0) { return -EFAULT; }
    return 0;
}

// get_mempolicy(mode, nodemask, maxnode, addr, flags) -- Linux shape.
// NeoOS has exactly one NUMA node: `mode` (if given) is always written
// MPOL_DEFAULT (0), and `nodemask` (if given, and maxnode >= 1) is
// written with exactly bit 0 set -- node 0 is the only node there is.
// `addr`/MPOL_F_ADDR is not interpreted: with one node, "which node is
// this address on" has only one possible answer regardless of which
// address is asked about.
#define MPOL_DEFAULT 0

// getrusage(who, usage) -- Linux's struct rusage shape exactly
// (two timeval's -- user/system CPU time -- then fourteen longs).
// ru_utime is the calling process's real CPU time (see below); every
// other field is zero: NeoOS has no page-fault or context-switch
// counters to report honestly instead. `who` (RUSAGE_SELF/CHILDREN/
// THREAD) is accepted and makes no difference. Zero reads as
// "unknown", which is the truth, not a fabricated measurement.
// Found missing (fatal) getting a real ASP.NET Core app running --
// the GC's own diagnostics call it during startup.
struct neoos_rusage {
    int64_t ru_utime_sec, ru_utime_usec;
    int64_t ru_stime_sec, ru_stime_usec;
    int64_t fields[14];
};

int64_t sys_getrusage(struct syscall_args *a) {
    uint64_t out = a->a2;
    if (!out) { return -EFAULT; }

    struct neoos_rusage ru;
    for (unsigned i = 0; i < sizeof(ru); i++) { ((uint8_t *)&ru)[i] = 0; }

    // ru_utime is the process's real CPU time (user + kernel: the
    // scheduler does not split them). ru_stime stays zero.
    uint64_t us = process_cpu_ns(current_proc()) / 1000;
    ru.ru_utime_sec  = (int64_t)(us / 1000000);
    ru.ru_utime_usec = (int64_t)(us % 1000000);

    uint64_t missed = copy_to_user((void *)(uintptr_t)out, &ru, sizeof ru);
    if (missed > 0) { return -EFAULT; }
    return 0;
}

int64_t sys_get_mempolicy(struct syscall_args *a) {
    uint64_t mode_ptr     = a->a1;
    uint64_t nodemask_ptr = a->a2;
    uint64_t maxnode      = a->a3;

    if (mode_ptr) {
        int mode = MPOL_DEFAULT;
        uint64_t missed = copy_to_user((void *)(uintptr_t)mode_ptr, &mode, sizeof mode);
        if (missed > 0) { return -EFAULT; }
    }
    if (nodemask_ptr && maxnode >= 1) {
        uint64_t word = 1;   // node 0 only
        uint64_t missed = copy_to_user((void *)(uintptr_t)nodemask_ptr, &word, sizeof word);
        if (missed > 0) { return -EFAULT; }
    }
    return 0;
}
