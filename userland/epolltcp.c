// userland/epolltcp.c -- epoll-driven TCP accept/echo loop.
//
// The .NET-free oracle for the ASP.NET Core / Kestrel side of the
// concurrent-request crash investigation (docs/superpowers/specs/
// 2026-09-07-concurrent-request-crash-investigation.md), the way
// mmstress.c is the oracle for the mm side.
//
// Kestrel does not accept with a blocking accept() on a thread. Its
// SocketAsyncEngine puts the LISTENING socket in an epoll set, waits,
// and accepts only when epoll reports it readable -- then does the same
// for every accepted connection. A raw blocking TcpListener on the same
// machine served requests fine while Kestrel answered nothing, which
// says the difference is that path, not TCP.
//
// So this is that path and nothing else: one epoll set, a non-blocking
// listener registered in it, connections accepted from the epoll
// readiness report, registered in turn, and answered with a fixed HTTP
// response when THEY report readable. No runtime, no GC, no threads --
// if this serves N concurrent connections and .NET does not, the bug is
// above the kernel; if it hangs the way Kestrel hangs, the bug is here
// and this reproduces it in a 25-second boot.
//
// Run headless: `make epolltcp` (its own boot, INITTAB runs only this).
// The host side of the test drives it -- see the Makefile target.
// Green looks like: [epolltcp] ALL PASSED

#include <stdint.h>
#include <stdio.h>

// ---- raw syscalls (libneoos has no epoll/socket wrappers) -------------
// NeoOS's own numbers, from kernel/syscall/syscall_nr.h.
#define SYS_WRITE            1
#define SYS_READ             6
#define SYS_EXIT             0
#define SYS_CLOSE            8
#define SYS_SOCKET          45
#define SYS_BIND            46
#define SYS_LISTEN          83
#define SYS_ACCEPT4         84
#define SYS_SETSOCKOPT      87
#define SYS_EPOLL_CREATE1  100
#define SYS_EPOLL_CTL      101
#define SYS_EPOLL_WAIT     102

#define AF_INET        2
#define SOCK_STREAM    1
#define SOL_SOCKET     1
#define SO_REUSEADDR   2

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define SOCK_NONBLOCK 04000
#define EPOLLIN       0x001
#define EPOLLOUT      0x004
#define EPOLLERR      0x008
#define EPOLLHUP      0x010

// Linux's layout: __u32 events then a 64-bit union, packed.
struct epoll_event_abi { uint32_t events; uint64_t data; } __attribute__((packed));

struct sockaddr_in_abi {
    uint16_t sin_family;
    uint16_t sin_port;      // network byte order
    uint32_t sin_addr;      // network byte order
    uint8_t  pad[8];
};

static inline int64_t sc1(int64_t n, int64_t a) {
    int64_t r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory");
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
static inline int64_t sc5(int64_t n, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) {
    int64_t r; register int64_t r10 __asm__("r10") = d; register int64_t r8 __asm__("r8") = e;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8):"rcx","r11","memory");
    return r;
}

#define PORT        30000
#define MAXEV       32
#define WANT_CONNS  16     // the host driver sends this many requests

static const char RESP[] =
    "HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nhello neoos\n";

static uint16_t hton16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

int main(void) {
    printf("[epolltcp] start: epoll-driven accept loop on port %d, want %d conns\n",
           PORT, WANT_CONNS);

    int lfd = (int)sc3(SYS_SOCKET, AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { printf("[epolltcp] FAIL socket rc=%d\n", lfd); return 1; }

    int one = 1;
    sc5(SYS_SETSOCKOPT, lfd, SOL_SOCKET, SO_REUSEADDR,
        (int64_t)(uintptr_t)&one, sizeof one);

    struct sockaddr_in_abi sa = { 0 };
    sa.sin_family = AF_INET;
    sa.sin_port   = hton16(PORT);
    sa.sin_addr   = 0;                      // INADDR_ANY
    int rc = (int)sc3(SYS_BIND, lfd, (int64_t)(uintptr_t)&sa, sizeof sa);
    if (rc < 0) { printf("[epolltcp] FAIL bind rc=%d\n", rc); return 1; }

    rc = (int)sc3(SYS_LISTEN, lfd, 16, 0);
    if (rc < 0) { printf("[epolltcp] FAIL listen rc=%d\n", rc); return 1; }

    int ep = (int)sc1(SYS_EPOLL_CREATE1, 0);
    if (ep < 0) { printf("[epolltcp] FAIL epoll_create1 rc=%d\n", ep); return 1; }

    // The listener goes in the set. This is the step a blocking
    // accept() loop never takes, and the one Kestrel depends on.
    struct epoll_event_abi ev;
    ev.events = EPOLLIN;
    ev.data   = (uint64_t)lfd;
    rc = (int)sc4(SYS_EPOLL_CTL, ep, EPOLL_CTL_ADD, lfd, (int64_t)(uintptr_t)&ev);
    if (rc < 0) { printf("[epolltcp] FAIL epoll_ctl ADD listener rc=%d\n", rc); return 1; }

    printf("[epolltcp] listening\n");

    int served = 0, accepted = 0, idle_rounds = 0;
    struct epoll_event_abi evs[MAXEV];

    while (served < WANT_CONNS) {
        int n = (int)sc4(SYS_EPOLL_WAIT, ep, (int64_t)(uintptr_t)evs, MAXEV, 1000);
        if (n < 0) { printf("[epolltcp] FAIL epoll_wait rc=%d\n", n); return 1; }
        if (n == 0) {
            // 60 consecutive second-long timeouts with nothing to do is
            // the hang this test exists to catch, not a slow host.
            if (++idle_rounds > 60) {
                printf("[epolltcp] FAIL timeout: accepted=%d served=%d of %d"
                       " -- epoll never reported the listener readable\n",
                       accepted, served, WANT_CONNS);
                return 1;
            }
            continue;
        }
        idle_rounds = 0;

        for (int i = 0; i < n; i++) {
            int fd = (int)evs[i].data;

            if (fd == lfd) {
                // Drain the backlog: level-triggered, but accepting only
                // one per report is how a real engine falls behind.
                for (;;) {
                    // SOCK_NONBLOCK, so a drained backlog is -EAGAIN
                    // rather than a block. NeoOS applies this flag to
                    // the accept AND to the accepted socket; Linux
                    // applies it only to the accepted socket and takes
                    // the blocking decision from the listener's own
                    // O_NONBLOCK (see docs/stdlib.md).
                    int c = (int)sc4(SYS_ACCEPT4, lfd, 0, 0, SOCK_NONBLOCK);
                    if (c < 0) { break; }        // EAGAIN: backlog drained
                    accepted++;
                    struct epoll_event_abi cev;
                    cev.events = EPOLLIN;
                    cev.data   = (uint64_t)c;
                    if (sc4(SYS_EPOLL_CTL, ep, EPOLL_CTL_ADD, c,
                            (int64_t)(uintptr_t)&cev) < 0) {
                        printf("[epolltcp] FAIL epoll_ctl ADD conn fd=%d\n", c);
                        return 1;
                    }
                }
                continue;
            }

            // A connection reported readable: read the request and answer.
            char buf[512];
            int64_t got = sc3(SYS_READ, fd, (int64_t)(uintptr_t)buf, sizeof buf);
            if (got > 0) {
                sc3(SYS_WRITE, fd, (int64_t)(uintptr_t)RESP, sizeof(RESP) - 1);
                served++;
            }
            // Close WITHOUT EPOLL_CTL_DEL first -- exactly what .NET's
            // SocketAsyncEngine does, and what epoll_forget_fd exists to
            // survive (the fd number is reused by the next connection).
            sc4(SYS_EPOLL_CTL, ep, EPOLL_CTL_DEL, fd, (int64_t)(uintptr_t)&ev);
            sc1(SYS_CLOSE, fd);
        }
    }

    printf("[epolltcp] accepted=%d served=%d\n", accepted, served);
    printf("[epolltcp] ALL PASSED\n");
    return 0;
}
