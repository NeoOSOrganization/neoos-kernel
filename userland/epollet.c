// userland/epollet.c -- EPOLLET (edge-triggered epoll) conformance test.
//
// The oracle for the ASP.NET Core / Kestrel concurrency hang. .NET's
// SocketAsyncEngine registers EVERY socket edge-triggered
// (events = 0x80000005 = EPOLLET|EPOLLOUT|EPOLLIN) and its loop is
// "report -> hand the fd to the thread pool -> straight back to
// epoll_wait". Under level-triggered delivery the fd is still ready
// when the engine gets back -- the work item has not run yet -- so it
// is dispatched again and again, the pool fills with duplicates, and
// accepted connections are never served. LT is NOT a safe superset for
// a consumer written against ET.
//
// So this is edge semantics and nothing else, on a pipe and on a
// socketpair, with no network and no host driver: `make epollet`, its
// own boot, 100 ms timeouts so a regression fails instead of hanging.
// Green looks like: [epollet] ALL PASSED
//
// What each case pins down:
//   1  level-triggered is unchanged        (no ET bit -> report repeats)
//   2  ET reports a readiness rise once    (the bug: this repeated)
//   3  more data is a NEW edge
//   4  drain then refill re-arms
//   5  EPOLL_CTL_MOD re-arms, per epoll(7)
//   6  DEL + re-ADD re-arms (a reused fd must not inherit a stale edge)
//   7  .NET's exact mask: EPOLLOUT does not repeat forever
//   8  drain and refill BETWEEN waits is still an edge (a sampling-only
//      implementation misses this one and hangs)

#include <stdint.h>
#include <stdio.h>

// ---- raw syscalls (libneoos has no epoll wrappers) --------------------
// NeoOS's own numbers, from kernel/syscall/syscall_nr.h.
#define SYS_WRITE            1
#define SYS_READ             6
#define SYS_CLOSE            8
#define SYS_PIPE2           43
#define SYS_SOCKETPAIR      91
#define SYS_EPOLL_CREATE1  100
#define SYS_EPOLL_CTL      101
#define SYS_EPOLL_WAIT     102

#define AF_UNIX        1
#define SOCK_STREAM    1

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN       0x001u
#define EPOLLOUT      0x004u
#define EPOLLERR      0x008u
#define EPOLLHUP      0x010u
#define EPOLLET       0x80000000u

#define WAIT_MS 100     // long enough for a real report, short enough to fail fast

// Linux's layout: __u32 events then a 64-bit union, packed.
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

static int failures;

static void fail(const char *what, int64_t got, int64_t want) {
    printf("[epollet] FAIL %s: got %d, want %d\n", what, (int)got, (int)want);
    failures++;
}

static int ep_ctl(int ep, int op, int fd, uint32_t events) {
    struct epoll_event_abi ev;
    ev.events = events;
    ev.data   = (uint64_t)fd;
    return (int)sc4(SYS_EPOLL_CTL, ep, op, fd, (int64_t)(uintptr_t)&ev);
}

// Returns the number of events reported, and the first event's mask in
// *mask (0 if none).
static int ep_wait(int ep, uint32_t *mask) {
    struct epoll_event_abi evs[8];
    int n = (int)sc4(SYS_EPOLL_WAIT, ep, (int64_t)(uintptr_t)evs, 8, WAIT_MS);
    if (mask) { *mask = (n > 0) ? evs[0].events : 0; }
    return n;
}

// Expect that whatever comes back does NOT carry `bits`. Weaker than
// expect(..., 0, ...) on purpose: a re-arm is object-wide, so an
// unrelated readiness change on the same object (a read freeing space
// for the peer) can re-report a bit that is still merely level-ready.
// Linux's ET has the same shape -- its wakeup callback is per-socket
// too -- and the property that MATTERS is this one: the bit a consumer
// dispatches on must not come back until it really transitioned.
static void expect_without(const char *what, int ep, uint32_t bits) {
    uint32_t mask = 0;
    printf("[epollet] ..   %s\n", what);
    int n = ep_wait(ep, &mask);
    if (n > 0 && (mask & bits)) {
        printf("[epollet] FAIL %s: mask 0x%x still carries 0x%x\n", what,
               (unsigned)mask, (unsigned)bits);
        failures++;
        return;
    }
    printf("[epollet] ok   %s\n", what);
}

// Expect exactly `want` events, and (when want > 0) `bits` set in the mask.
static void expect(const char *what, int ep, int want, uint32_t bits) {
    uint32_t mask = 0;
    // Announced before the wait, not just after it: if a regression
    // blocks in epoll_wait the log then says which check it died on
    // instead of just stopping.
    printf("[epollet] ..   %s\n", what);
    int n = ep_wait(ep, &mask);
    if (n != want) { fail(what, n, want); return; }
    if (want > 0 && bits && !(mask & bits)) {
        printf("[epollet] FAIL %s: mask 0x%x lacks 0x%x\n", what,
               (unsigned)mask, (unsigned)bits);
        failures++;
        return;
    }
    printf("[epollet] ok   %s\n", what);
}

static void put(int fd, const char *s, int len) {
    int64_t rc = sc3(SYS_WRITE, fd, (int64_t)(uintptr_t)s, len);
    if (rc != len) { fail("write", rc, len); }
}

// Read until it would block, so the readiness LEVEL is gone.
static void drain(int fd) {
    char buf[256];
    for (;;) {
        int64_t got = sc3(SYS_READ, fd, (int64_t)(uintptr_t)buf, sizeof buf);
        if (got <= 0) { return; }
        if (got < (int64_t)sizeof buf) { return; }
    }
}

int main(void) {
    printf("[epollet] start: edge-triggered epoll conformance\n");

    int ep = (int)sc1(SYS_EPOLL_CREATE1, 0);
    if (ep < 0) { printf("[epollet] FAIL epoll_create1 rc=%d\n", ep); return 1; }

    // ---- 1: level-triggered is unchanged ------------------------------
    // The control case. Without EPOLLET the same unread byte must be
    // reported every time, which is what every existing NeoOS poller
    // (and userland/epolltcp.c) depends on.
    {
        int fds[2];
        if (sc2(SYS_PIPE2, (int64_t)(uintptr_t)fds, 0) < 0) {
            printf("[epollet] FAIL pipe2\n"); return 1;
        }
        if (ep_ctl(ep, EPOLL_CTL_ADD, fds[0], EPOLLIN) < 0) {
            printf("[epollet] FAIL ADD lt\n"); return 1;
        }
        put(fds[1], "x", 1);
        expect("1a LT reports readable", ep, 1, EPOLLIN);
        expect("1b LT reports it AGAIN (level, undrained)", ep, 1, EPOLLIN);
        ep_ctl(ep, EPOLL_CTL_DEL, fds[0], 0);
        sc1(SYS_CLOSE, fds[0]); sc1(SYS_CLOSE, fds[1]);
    }

    // ---- 2-6: edge-triggered on a pipe --------------------------------
    {
        int fds[2];
        if (sc2(SYS_PIPE2, (int64_t)(uintptr_t)fds, 0) < 0) {
            printf("[epollet] FAIL pipe2 et\n"); return 1;
        }
        int rd = fds[0], wr = fds[1];
        if (ep_ctl(ep, EPOLL_CTL_ADD, rd, EPOLLIN | EPOLLET) < 0) {
            printf("[epollet] FAIL ADD et\n"); return 1;
        }

        // 2: one rise, one report.
        put(wr, "a", 1);
        expect("2a ET reports the rise", ep, 1, EPOLLIN);
        expect("2b ET does NOT repeat it", ep, 0, 0);

        // 3: more data while the old data is still unread is a new edge.
        put(wr, "b", 1);
        expect("3  more data is a new edge", ep, 1, EPOLLIN);
        expect("3b and then quiet again", ep, 0, 0);

        // 4: drain to empty, refill -- the classic ET cycle.
        drain(rd);
        expect("4a drained: nothing to report", ep, 0, 0);
        put(wr, "c", 1);
        expect("4b refill after drain is an edge", ep, 1, EPOLLIN);

        // 5: EPOLL_CTL_MOD re-arms even with the same mask and no new
        //    data -- epoll(7) says so, and it is how a consumer that
        //    lost track recovers.
        expect("5a quiet before MOD", ep, 0, 0);
        if (ep_ctl(ep, EPOLL_CTL_MOD, rd, EPOLLIN | EPOLLET) < 0) {
            printf("[epollet] FAIL MOD\n"); return 1;
        }
        expect("5b MOD re-arms the edge", ep, 1, EPOLLIN);

        // 6: DEL + re-ADD is a fresh registration. A reused fd number
        //    (exactly what .NET does with connection sockets) must not
        //    inherit the previous registration's consumed edge.
        expect("6a quiet before re-ADD", ep, 0, 0);
        ep_ctl(ep, EPOLL_CTL_DEL, rd, 0);
        if (ep_ctl(ep, EPOLL_CTL_ADD, rd, EPOLLIN | EPOLLET) < 0) {
            printf("[epollet] FAIL re-ADD\n"); return 1;
        }
        expect("6b re-ADD re-arms the edge", ep, 1, EPOLLIN);

        ep_ctl(ep, EPOLL_CTL_DEL, rd, 0);
        sc1(SYS_CLOSE, rd); sc1(SYS_CLOSE, wr);
    }

    // ---- 7-8: .NET's exact mask on a stream socket --------------------
    {
        int sv[2];
        if (sc4(SYS_SOCKETPAIR, AF_UNIX, SOCK_STREAM, 0,
                (int64_t)(uintptr_t)sv) < 0) {
            printf("[epollet] FAIL socketpair\n"); return 1;
        }
        // 0x80000005, byte for byte what SocketAsyncEngine registers.
        if (ep_ctl(ep, EPOLL_CTL_ADD, sv[0], EPOLLIN | EPOLLOUT | EPOLLET) < 0) {
            printf("[epollet] FAIL ADD sock\n"); return 1;
        }

        // 7: an empty send buffer is writable forever. Under LT that
        //    alone returns from every epoll_wait immediately -- the
        //    engine never sleeps and never stops dispatching.
        expect("7a ET reports writable once", ep, 1, EPOLLOUT);
        expect("7b writable does not repeat", ep, 0, 0);

        // 8: the case a sample-only ET implementation gets wrong. The
        //    level drops and rises entirely BETWEEN two epoll_waits, so
        //    comparing "ready now" against "ready last time" sees no
        //    change and reports nothing -- and the reader hangs on data
        //    that is sitting right there.
        put(sv[1], "req1", 4);
        expect("8a first request is an edge", ep, 1, EPOLLIN);
        drain(sv[0]);
        put(sv[1], "req2", 4);
        expect("8b drain+refill between waits is an edge", ep, 1, EPOLLIN);
        drain(sv[0]);
        // Not "no events": the read itself freed buffer space, which is
        // a real readiness change on this object and re-arms it, so
        // EPOLLOUT may legitimately come back. What must NOT come back
        // is EPOLLIN -- a duplicate readable dispatch on a socket with
        // nothing to read is precisely what filled Kestrel's thread
        // pool with work items that did nothing.
        expect_without("8c drained: not readable again", ep, EPOLLIN);

        ep_ctl(ep, EPOLL_CTL_DEL, sv[0], 0);
        sc1(SYS_CLOSE, sv[0]); sc1(SYS_CLOSE, sv[1]);
    }

    sc1(SYS_CLOSE, ep);

    if (failures) {
        printf("[epollet] FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("[epollet] ALL PASSED\n");
    return 0;
}
