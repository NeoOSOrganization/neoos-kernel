// kernel/syscall/sys_poll.c -- poll(2) and select(2).
//
// Both compile down to one core: scan every fd's file_ops.poll for
// readiness; if none is ready, sleep on the global poll broadcast
// (waitq.c) until any system-wide readiness change or the timeout, then
// re-scan. See docs/stdlib.md for the divergences (flag subset, the
// nfds cap, the broadcast wake).

#include "syscall/syscall_internal.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/file.h"
#include "sync/waitq.h"
#include "drivers/char/timer.h"
#include "mm/paging.h"
#include "errno.h"

#define POLL_MAX_FDS 16   // nfds ceiling; the M1a terminal polls two

struct pollfd { int fd; short events; short revents; };

// fd_set: Linux's 1024-bit mask, i.e. 16 x uint64_t.
#define FD_SETSIZE 1024
#define FD_WORDS   (FD_SETSIZE / 64)

static int64_t poll_core(struct pollfd *pfd, unsigned n, int64_t deadline) {
    struct process *p = current_proc();

    waitq_poll_enter();
    int64_t ready = 0;
    for (;;) {
        ready = 0;
        for (unsigned i = 0; i < n; i++) {
            if (pfd[i].fd < 0) { pfd[i].revents = 0; continue; }
            struct file_descriptor *f = fd_get(p, pfd[i].fd);
            short want = pfd[i].events | POLLERR | POLLHUP;
            short got  = f ? (short)file_poll(f, want) : POLLNVAL;
            pfd[i].revents = got & (pfd[i].events | POLLERR | POLLHUP | POLLNVAL);
            if (pfd[i].revents) { ready++; }
        }
        if (ready) { break; }
        if (deadline == 0) { break; }                 // timeout 0 = non-blocking scan
        int rc = waitq_poll_wait((uint64_t)deadline);
        if (rc == -EINTR) { waitq_poll_leave(); return -EINTR; }
        if (rc == -ETIMEDOUT) { break; }
        // otherwise woken by a broadcast -- loop and re-scan
    }
    waitq_poll_leave();
    return ready;
}

// timeout_ms < 0 -> block forever; 0 -> non-blocking; else a deadline in
// timer ticks (one tick = 10 ms).
static int64_t deadline_from_ms(int timeout_ms) {
    if (timeout_ms < 0)  { return (int64_t)UINT64_MAX; }
    if (timeout_ms == 0) { return 0; }
    uint64_t ticks = (uint64_t)timeout_ms / 10;
    if (ticks == 0) { ticks = 1; }
    return (int64_t)(timer_ticks() + ticks);
}

int64_t sys_poll(struct syscall_args *a) {
    uint64_t uptr = a->a1;
    unsigned n    = (unsigned)a->a2;
    int      tmo  = (int)a->a3;

    if (n > POLL_MAX_FDS) { return -EINVAL; }
    if (n == 0) { return 0; }
    if (!user_range_writable(uptr, n * sizeof(struct pollfd))) { return -EFAULT; }

    struct pollfd pfd[POLL_MAX_FDS];
    for (unsigned i = 0; i < n; i++) {
        struct pollfd *up = (struct pollfd *)(uintptr_t)(uptr + i * sizeof(struct pollfd));
        pfd[i].fd     = up->fd;
        pfd[i].events = up->events;
        pfd[i].revents = 0;
    }

    int64_t r = poll_core(pfd, n, deadline_from_ms(tmo));
    if (r >= 0) {
        for (unsigned i = 0; i < n; i++) {
            struct pollfd *up = (struct pollfd *)(uintptr_t)(uptr + i * sizeof(struct pollfd));
            up->revents = pfd[i].revents;
        }
    }
    return r;
}

// select(nfds, readfds, writefds, exceptfds, timeval*)
// timeval is { long tv_sec; long tv_usec; }.
int64_t sys_select(struct syscall_args *a) {
    int nfds = (int)a->a1;
    uint64_t urd = a->a2, uwr = a->a3, uex = a->a4;
    uint64_t utv = a->frame->r8;

    if (nfds < 0 || nfds > FD_SETSIZE) { return -EINVAL; }

    uint64_t rd[FD_WORDS] = {0}, wr[FD_WORDS] = {0}, ex[FD_WORDS] = {0};
    if (urd) { if (!user_range_writable(urd, sizeof rd)) { return -EFAULT; }
               for (int i = 0; i < FD_WORDS; i++) { rd[i] = ((uint64_t *)(uintptr_t)urd)[i]; } }
    if (uwr) { if (!user_range_writable(uwr, sizeof wr)) { return -EFAULT; }
               for (int i = 0; i < FD_WORDS; i++) { wr[i] = ((uint64_t *)(uintptr_t)uwr)[i]; } }
    if (uex) { if (!user_range_writable(uex, sizeof ex)) { return -EFAULT; }
               for (int i = 0; i < FD_WORDS; i++) { ex[i] = ((uint64_t *)(uintptr_t)uex)[i]; } }

    // Collect the set bits into a pollfd array (capped like poll).
    struct pollfd pfd[POLL_MAX_FDS];
    unsigned n = 0;
    for (int fd = 0; fd < nfds && n < POLL_MAX_FDS; fd++) {
        int w = fd / 64, b = fd % 64;
        short ev = 0;
        if (rd[w] & (1ULL << b)) { ev |= POLLIN; }
        if (wr[w] & (1ULL << b)) { ev |= POLLOUT; }
        if (ex[w] & (1ULL << b)) { ev |= POLLERR; }
        if (!ev) { continue; }
        pfd[n].fd = fd; pfd[n].events = ev; pfd[n].revents = 0;
        n++;
    }

    int timeout_ms = -1;
    if (utv) {
        if (!user_range_writable(utv, 16)) { return -EFAULT; }
        long sec  = ((long *)(uintptr_t)utv)[0];
        long usec = ((long *)(uintptr_t)utv)[1];
        timeout_ms = (int)(sec * 1000 + usec / 1000);
        if (timeout_ms < 0) { timeout_ms = 0; }
    }

    int64_t r = poll_core(pfd, n, deadline_from_ms(timeout_ms));
    if (r < 0) { return r; }

    // Rebuild the sets from revents.
    for (int i = 0; i < FD_WORDS; i++) { rd[i] = wr[i] = ex[i] = 0; }
    int64_t count = 0;
    for (unsigned i = 0; i < n; i++) {
        int fd = pfd[i].fd, w = fd / 64, b = fd % 64;
        if (pfd[i].revents & POLLIN)  { rd[w] |= (1ULL << b); count++; }
        if (pfd[i].revents & POLLOUT) { wr[w] |= (1ULL << b); count++; }
        if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { ex[w] |= (1ULL << b); count++; }
    }
    if (urd) { for (int i = 0; i < FD_WORDS; i++) { ((uint64_t *)(uintptr_t)urd)[i] = rd[i]; } }
    if (uwr) { for (int i = 0; i < FD_WORDS; i++) { ((uint64_t *)(uintptr_t)uwr)[i] = wr[i]; } }
    if (uex) { for (int i = 0; i < FD_WORDS; i++) { ((uint64_t *)(uintptr_t)uex)[i] = ex[i]; } }
    return count;
}
