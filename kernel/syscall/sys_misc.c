// kernel/syscall/sys_misc.c -- SMP visibility and futex.
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
#include "dev/rtc.h"
#include "mm/vma.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "arch/cpu_local.h"
#include "smp/smp.h"
#include "net/socket.h"

int64_t sys_cpu_count(struct syscall_args *a) {
    (void)a;
    return smp_online_count();
}

int64_t sys_getcpu(struct syscall_args *a) {
    (void)a;
    return (int)(this_cpu() - &cpus[0]);
}

int64_t sys_futex(struct syscall_args *a) {
    // Linux's argument order, unchanged: uaddr, op, val, timeout. The
    // fifth and sixth (uaddr2, val3) belong to REQUEUE and the BITSET
    // operations, neither of which is implemented, so they are not read.
    return futex_op((uint32_t *)(uintptr_t)a->a1, (int)a->a2, (uint32_t)a->a3,
                    (const struct k_timespec *)(uintptr_t)a->a4);
}

// ---- the clock -------------------------------------------------------
//
// NeoOS's only fine time source is the 100Hz tick counter the local
// APIC timer advances, so the resolution is 10ms. CLOCK_REALTIME is
// wall time, anchored to the CMOS RTC read once at boot (dev/rtc.c);
// CLOCK_MONOTONIC and the CPU-time clocks count from boot.
//
// DIVERGENCE, recorded in docs/stdlib.md: 10ms resolution, no absolute
// timeouts, and if the RTC could not be read at boot CLOCK_REALTIME
// silently falls back to a boot epoch and formats as January 1970.

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4

#define TICK_HZ     100
#define NS_PER_TICK (1000000000ULL / TICK_HZ)

int64_t sys_clock_gettime(struct syscall_args *a) {
    int clk = (int)a->a1;
    struct k_timespec *out = (struct k_timespec *)(uintptr_t)a->a2;
    if (!out) { return -EFAULT; }

    switch (clk) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        break;
    default:
        return -EINVAL;
    }

    uint64_t ticks = timer_ticks();
    int64_t  sec   = (int64_t)(ticks / TICK_HZ);
    int64_t  nsec  = (int64_t)((ticks % TICK_HZ) * NS_PER_TICK);

    // CLOCK_REALTIME is wall time, anchored to the CMOS RTC read at
    // boot; CLOCK_MONOTONIC counts from boot. They are different
    // clocks now, which they were not when both were tick counters.
    if (clk == CLOCK_REALTIME) { sec += rtc_boot_epoch(); }

    out->tv_sec  = sec;
    out->tv_nsec = nsec;
    return 0;
}

// Relative sleep, rounded UP to a whole tick: sleeping less than asked
// is a bug a caller cannot defend against, whereas sleeping slightly
// longer is what every tick-driven kernel does.
//
// DIVERGES: the remaining-time argument is ignored, because nothing
// here can interrupt a sleep partway and report a remainder yet.
int64_t sys_nanosleep(struct syscall_args *a) {
    const struct k_timespec *req = (const struct k_timespec *)(uintptr_t)a->a1;
    if (!req) { return -EFAULT; }
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) {
        return -EINVAL;
    }

    uint64_t ns    = (uint64_t)req->tv_sec * 1000000000ULL + (uint64_t)req->tv_nsec;
    uint64_t ticks = (ns + NS_PER_TICK - 1) / NS_PER_TICK;
    if (ticks == 0 && ns > 0) { ticks = 1; }

    uint64_t deadline = timer_ticks() + ticks;
    while (timer_ticks() < deadline) {
        schedule();
    }
    return 0;
}
