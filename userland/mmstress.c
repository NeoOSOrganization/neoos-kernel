// userland/mmstress.c -- concurrent address-space stress test.
//
// The fast, .NET-free oracle for the concurrent-request crash
// investigation (docs/superpowers/specs/2026-09-07-concurrent-request-
// crash-investigation.md, Phase 1a). Many threads of ONE process
// hammer mmap / munmap / mprotect / page faults on private anonymous
// memory while writing and verifying a per-thread pattern. If the
// kernel's mm is not SMP-safe for multiple threads of one process, a
// verify mismatch or a SIGSEGV falls out here in seconds -- with no GC
// and no runtime in the way.
//
// Phase 1a "escalate" branch: pure mm exposed the TLB deferred-free
// orphan-frame leak (fixed in kernel/smp/tlb.c -- see the spec). This
// build adds the concurrency the .NET webtest actually has and the pure
// path did not:
//   - epoll churn: threads that create pipes, EPOLL_CTL_ADD/DEL them,
//     epoll_wait(timeout 0), and close() them in a tight loop, against
//     one shared epoll fd -- exercises the epoll registry + the
//     epoll_forget_fd(close) path + poll_core over a racing fd set.
//   - brk churn: currently inert (sys_brk is a stub); still a thread +
//     syscall stressor.
//   - a fork-COW phase at the end: fork children that verify a shared
//     pre-populated region (write-faulting every page => COW), while mm
//     threads keep hammering.
// Each block is guarded by a MMS_* compile knob (all on by default) so
// a red run can be bisected by rebuilding with one turned off:
//   make mmstress SMP_CPUS=4 EXTRA_CFLAGS='-DMMS_EPOLL=0'
//
// Run headless: `make mmstress` (its own boot, INITTAB runs only this).
// Green looks like: [mmstress] ALL PASSED
// Red looks like:   [mmstress] FAIL ... / a [fault-audit] line / a
//                   kernel [exception].

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>

#ifndef MMS_EPOLL
#define MMS_EPOLL 1
#endif
#ifndef MMS_BRK
#define MMS_BRK 1
#endif
#ifndef MMS_FORK
#define MMS_FORK 1
#endif

#define NTHREADS      8
#define ITERS         600      // per mm thread
#define PAGE          4096

// ---- raw syscalls (libneoos has no epoll/brk wrapper) ------------------
// NeoOS's own numbers, from kernel/syscall/syscall_nr.h.
#define SYS_EXIT            0
#define SYS_CLOSE           8
#define SYS_FORK            12
#define SYS_WAIT4           32
#define SYS_PIPE2           43
#define SYS_BRK             75
#define SYS_EPOLL_CREATE1   100
#define SYS_EPOLL_CTL       101
#define SYS_EPOLL_WAIT      102

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLLIN       0x001

struct epoll_event_abi { uint32_t events; uint64_t data; } __attribute__((packed));

static inline int64_t sc1(int64_t n, int64_t a) {
    int64_t r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory");
    return r;
}
static inline int64_t sc2(int64_t n, int64_t a, int64_t b) {
    int64_t r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b):"rcx","r11","memory");
    return r;
}
static inline int64_t sc3(int64_t n, int64_t a, int64_t b, int64_t c) {
    int64_t r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory");
    return r;
}
static inline int64_t sc4(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d) {
    int64_t r; register int64_t r10 __asm__("r10") = d;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory");
    return r;
}

__attribute__((unused))
static void die(int code) {
    __asm__ volatile("syscall" :: "a"((int64_t)SYS_EXIT), "D"((int64_t)code) : "rcx", "r11", "memory");
    for (;;) { }
}

static volatile int g_failed;
static volatile int g_stop;      // tells the churn threads to wind down

// libneoos printf has no %p / length modifiers -- split 64-bit values
// into two %x halves by hand.
static void fail(const char *what, void *p, uint64_t a, uint64_t b) {
    uint64_t pv = (uint64_t)(uintptr_t)p;
    g_failed = 1;
    printf("[mmstress] FAIL %s p=%x:%x a=%x:%x b=%x:%x\n", what,
           (unsigned)(pv >> 32), (unsigned)pv,
           (unsigned)(a >> 32), (unsigned)a,
           (unsigned)(b >> 32), (unsigned)b);
}

// Fill every page of [p, p+len) with a word derived from (tag, page
// index), then read it all back and check. `toggle` also flips the
// region to PROT_READ and back mid-way, exercising mprotect + the
// cross-CPU TLB flush against the concurrent writers on other threads.
static void bang(uint64_t tag, uint64_t len, int toggle) {
    unsigned char *p = mmap(0, len, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) { fail("mmap", 0, len, 0); return; }

    uint64_t npages = len / PAGE;
    for (uint64_t i = 0; i < npages; i++) {
        uint64_t *w = (uint64_t *)(p + i * PAGE);
        *w = tag ^ (i * 0x9E3779B97F4A7C15ULL);
        p[i * PAGE + PAGE - 1] = (unsigned char)(tag + i);   // touch the last byte too
    }

    if (toggle) {
        if (mprotect(p, len, PROT_READ) != 0) { fail("mprotect ro", p, len, 0); }
        if (mprotect(p, len, PROT_READ | PROT_WRITE) != 0) { fail("mprotect rw", p, len, 0); }
    }

    for (uint64_t i = 0; i < npages; i++) {
        uint64_t *w = (uint64_t *)(p + i * PAGE);
        uint64_t want = tag ^ (i * 0x9E3779B97F4A7C15ULL);
        if (*w != want) { fail("verify word", p, *w, want); break; }
        unsigned char cw = (unsigned char)(tag + i);
        if (p[i * PAGE + PAGE - 1] != cw) { fail("verify byte", p, i, 0); break; }
    }

    if (munmap(p, len) != 0) { fail("munmap", p, len, 0); }
}

static void *worker(void *arg) {
    uint64_t id = (uint64_t)(uintptr_t)arg;
    const uint64_t sizes[] = { PAGE, 16 * PAGE, 64 * PAGE, PAGE, 256 * PAGE };
    for (int it = 0; it < ITERS && !g_failed; it++) {
        uint64_t len = sizes[it % 5];
        uint64_t tag = (id << 40) ^ ((uint64_t)it << 8) ^ 0xC0FFEE;
        bang(tag, len, (it & 3) == 0);
    }
    return 0;
}

#if MMS_EPOLL
// One shared epoll fd; every churn thread races ADD/DEL/close on it,
// mirroring .NET's SocketAsyncEngine (one engine epoll, many threadpool
// threads touching it).
static int g_epfd = -1;

static void *epoll_churn(void *arg) {
    uint64_t id = (uint64_t)(uintptr_t)arg;
    for (int it = 0; it < 4000 && !g_failed && !g_stop; it++) {
        int fds[2];
        if (sc2(SYS_PIPE2, (int64_t)(uintptr_t)fds, 0) != 0) { continue; }

        struct epoll_event_abi ev = { .events = EPOLLIN,
                                      .data = (id << 32) | (uint32_t)it };
        sc4(SYS_EPOLL_CTL, g_epfd, EPOLL_CTL_ADD, fds[0], (int64_t)(uintptr_t)&ev);
        sc4(SYS_EPOLL_CTL, g_epfd, EPOLL_CTL_ADD, fds[1], (int64_t)(uintptr_t)&ev);

        struct epoll_event_abi out[8];
        sc4(SYS_EPOLL_WAIT, g_epfd, (int64_t)(uintptr_t)out, 8, 0);

        // Half the time DEL first, half the time just close and let
        // epoll_forget_fd(close) do it -- both are paths .NET hits.
        if (it & 1) {
            sc4(SYS_EPOLL_CTL, g_epfd, EPOLL_CTL_DEL, fds[0], 0);
            sc4(SYS_EPOLL_CTL, g_epfd, EPOLL_CTL_DEL, fds[1], 0);
        }
        sc1(SYS_CLOSE, fds[0]);
        sc1(SYS_CLOSE, fds[1]);
    }
    return 0;
}
#endif

#if MMS_BRK
// NOTE: NeoOS's sys_brk is deliberately a stub that always returns the
// current break unchanged (musl falls back to mmap) -- so this loop
// currently only adds a thread and a stream of syscalls, never actually
// grows the heap. Kept because "epoll churn + one extra thread" at
// -smp 2 is itself a useful stressor, and this becomes a real test the
// day sys_brk grows a heap.
static void *brk_churn(void *arg) {
    (void)arg;
    uint64_t base = (uint64_t)sc1(SYS_BRK, 0);
    if (!base) { return 0; }
    for (int it = 0; it < 4000 && !g_failed && !g_stop; it++) {
        uint64_t grow = ((it % 32) + 1) * PAGE;
        uint64_t top = (uint64_t)sc1(SYS_BRK, (int64_t)(base + grow));
        if (top >= base + grow) {
            volatile unsigned char *q = (unsigned char *)base;
            for (uint64_t o = 0; o < grow; o += PAGE) { q[o] = (unsigned char)it; }
            for (uint64_t o = 0; o < grow; o += PAGE) {
                if (q[o] != (unsigned char)it) { fail("brk verify", (void *)base, o, it); break; }
            }
        }
        sc1(SYS_BRK, (int64_t)base);   // shrink back
    }
    return 0;
}
#endif

#if MMS_FORK
// A shared, fully pre-populated region. Each forked child write-touches
// every page (COW-faulting a private copy) and verifies, while the mm
// worker threads keep hammering their own regions in the parent.
static void fork_cow_phase(void) {
    uint64_t len = 512 * PAGE;
    unsigned char *shared = mmap(0, len, PROT_READ | PROT_WRITE,
                                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (shared == MAP_FAILED) { fail("cow mmap", 0, len, 0); return; }
    for (uint64_t i = 0; i < len / PAGE; i++) {
        *(uint64_t *)(shared + i * PAGE) = 0xA5A5A5A500000000ULL | i;
    }

    for (int round = 0; round < 16 && !g_failed; round++) {
        int64_t pid = sc1(SYS_FORK, 0);
        if (pid == 0) {
            for (uint64_t i = 0; i < len / PAGE; i++) {
                uint64_t *w = (uint64_t *)(shared + i * PAGE);
                if (*w != (0xA5A5A5A500000000ULL | i)) { die(2); }
                *w ^= 0xFFFFFFFFULL;              // COW write
            }
            for (uint64_t i = 0; i < len / PAGE; i++) {
                uint64_t *w = (uint64_t *)(shared + i * PAGE);
                if (*w != ((0xA5A5A5A500000000ULL | i) ^ 0xFFFFFFFFULL)) { die(3); }
            }
            die(0);
        } else if (pid > 0) {
            int st = 0;
            sc4(SYS_WAIT4, pid, (int64_t)(uintptr_t)&st, 0, 0);
            if (st != 0) { fail("cow child", 0, (uint64_t)pid, (uint64_t)st); }
            // Parent's copy must be untouched (child COW'd away).
            for (uint64_t i = 0; i < len / PAGE; i += 37) {
                if (*(uint64_t *)(shared + i * PAGE) != (0xA5A5A5A500000000ULL | i)) {
                    fail("cow parent", shared, i, round); break;
                }
            }
        }
    }
    munmap(shared, len);
}
#endif

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[mmstress] start: %d threads x %d iters, one address space "
           "(epoll=%d brk=%d fork=%d)\n",
           NTHREADS, ITERS, MMS_EPOLL, MMS_BRK, MMS_FORK);

    pthread_t churn[4];
    int nchurn = 0;

#if MMS_EPOLL
    g_epfd = (int)sc1(SYS_EPOLL_CREATE1, 0);
    if (g_epfd < 0) { printf("[mmstress] FAIL epoll_create1 %d\n", g_epfd); return 1; }
    pthread_create(&churn[nchurn++], 0, epoll_churn, (void *)0);
    pthread_create(&churn[nchurn++], 0, epoll_churn, (void *)1);
#endif
#if MMS_BRK
    pthread_create(&churn[nchurn++], 0, brk_churn, 0);
#endif

    pthread_t th[NTHREADS];
    for (uint64_t i = 0; i < NTHREADS; i++) {
        int rc = pthread_create(&th[i], 0, worker, (void *)(uintptr_t)i);
        if (rc != 0) {
            printf("[mmstress] FAIL pthread_create idx=%d rc=%d\n", (int)i, rc);
            return 1;
        }
    }
    for (int i = 0; i < NTHREADS; i++) { pthread_join(th[i], 0); }

    g_stop = 1;
    for (int i = 0; i < nchurn; i++) { pthread_join(churn[i], 0); }

#if MMS_FORK
    if (!g_failed) { fork_cow_phase(); }
#endif

#if MMS_EPOLL
    if (g_epfd >= 0) { sc1(SYS_CLOSE, g_epfd); }
#endif

    if (g_failed) { printf("[mmstress] FAILED\n"); return 1; }
    printf("[mmstress] ALL PASSED\n");
    return 0;
}
