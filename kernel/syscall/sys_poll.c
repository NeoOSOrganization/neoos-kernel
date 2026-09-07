// kernel/syscall/sys_poll.c -- poll(2) and select(2).
//
// Both compile down to one core: scan every fd's file_ops.poll for
// readiness; if none is ready, sleep on the global poll broadcast
// (waitq.c) until any system-wide readiness change or the timeout, then
// re-scan. See docs/stdlib.md for the divergences (flag subset, the
// broadcast wake, and poll()'s remaining nfds cap -- select() sizes its
// array to the caller's request and has no cap below FD_SETSIZE).

#include "syscall/syscall_internal.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "fs/file.h"
#include "sync/waitq.h"
#include "sync/poll_head.h"
#include "sync/epoll.h"
#include "sync/inotify.h"
#include "drivers/char/timer.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "mm/uaccess.h"
#include "errno.h"

// Stack budget for a small poll. Not a ceiling: past this the array is
// allocated. The ceiling is FD_TABLE_MAX, since polling more
// descriptors than the process can hold open is meaningless.
#define POLL_SMALL_FDS 16

struct pollfd { int fd; short events; short revents; };

// fd_set: Linux's 1024-bit mask, i.e. 16 x uint64_t.
#define FD_SETSIZE 1024
#define FD_WORDS   (FD_SETSIZE / 64)

static int64_t poll_core(struct pollfd *pfd, unsigned n, int64_t deadline) {
    struct process *p = current_proc();
    struct thread  *self = current_thread();

    // CS5.2: register on each polled object's own poll head, so only a
    // readiness change on something this call actually asked about wakes
    // it. Registrations live here, on this frame, for exactly the length
    // of the call -- which is why every exit path below unregisters.
    struct poll_reg  small_regs[POLL_SMALL_FDS];
    struct poll_reg *regs = small_regs;
    if (n > POLL_SMALL_FDS) {
        regs = kmalloc((uint64_t)n * sizeof(*regs));
        if (!regs) { return -ENOMEM; }
    }
    for (unsigned i = 0; i < n; i++) { regs[i].head = 0; regs[i].next = 0; regs[i].t = 0; }

    // Any fd whose object has no poll head yet leaves this poller on the
    // global broadcast. Objects are converted one at a time, so a mixed
    // set is the normal case during the transition and must be correct,
    // not merely tolerated.
    int wants_broadcast = 0;
    for (unsigned i = 0; i < n; i++) {
        if (pfd[i].fd < 0) { continue; }
        struct file_descriptor *f = fd_get(p, pfd[i].fd);
        struct poll_head *h = (f && f->ops && f->ops->poll_head) ? f->ops->poll_head(f) : 0;
        if (h) { poll_head_register(h, &regs[i], self); }
        else   { wants_broadcast = 1; }
    }
    __atomic_store_n(&self->poll_wants_broadcast, wants_broadcast, __ATOMIC_RELEASE);

    waitq_poll_enter();
    int64_t ready = 0;
    int woken = 0;
    for (;;) {
        // Cleared BEFORE the scan, not after: a notify that lands during
        // the scan must survive it, or this thread sleeps through the
        // very event it was told about.
        __atomic_store_n(&self->poll_notified, 0, __ATOMIC_RELEASE);

        ready = 0;
        for (unsigned i = 0; i < n; i++) {
            if (pfd[i].fd < 0) { pfd[i].revents = 0; continue; }
            struct file_descriptor *f = fd_get(p, pfd[i].fd);
            short want = pfd[i].events | POLLERR | POLLHUP;
            short got  = f ? (short)file_poll(f, want) : POLLNVAL;
            pfd[i].revents = got & (pfd[i].events | POLLERR | POLLHUP | POLLNVAL);
            if (pfd[i].revents) { ready++; }
        }
        if (woken) {
            // A wake that found nothing. Under the old broadcast this
            // was nearly every wake (266 of 276 over a boot); with poll
            // heads it should be close to none, and the number is how
            // that claim is checked rather than asserted.
            if (!ready) { waitq_poll_count_wasted(); }
            woken = 0;
        }
        if (ready) { break; }
        if (deadline == 0) { break; }                 // timeout 0 = non-blocking scan
        int rc = waitq_poll_wait((uint64_t)deadline, &self->poll_notified);
        if (rc == -EINTR) {
            waitq_poll_leave();
            for (unsigned i = 0; i < n; i++) { poll_head_unregister(&regs[i]); }
            __atomic_store_n(&self->poll_wants_broadcast, 0, __ATOMIC_RELEASE);
            if (regs != small_regs) { kfree(regs); }
            return -EINTR;
        }
        if (rc == -ETIMEDOUT) { break; }
        woken = 1;
    }
    waitq_poll_leave();
    for (unsigned i = 0; i < n; i++) { poll_head_unregister(&regs[i]); }
    __atomic_store_n(&self->poll_wants_broadcast, 0, __ATOMIC_RELEASE);
    if (regs != small_regs) { kfree(regs); }
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

    // The ceiling is the descriptor table's size, not a fixed 16.
    // sixteen was the M1a terminal's own need, and any program polling
    // more than that got -EINVAL -- a shell with a few jobs, or anything
    // built on a poll loop, is straight past it. select() was widened in
    // CS2.2; this is the same fix on the other call.
    if (n > FD_TABLE_MAX) { return -EINVAL; }
    if (n == 0) { return 0; }
    if (!uptr) { return -EFAULT; }

    // Small polls -- which is nearly all of them -- stay on the stack.
    // Only a large set pays for an allocation, and 16384 pollfds would
    // be 128 KiB, far past what a 16 KiB kernel stack can hold.
    struct pollfd small[POLL_SMALL_FDS];
    struct pollfd *pfd = small;
    if (n > POLL_SMALL_FDS) {
        pfd = kmalloc((uint64_t)n * sizeof(struct pollfd));
        if (!pfd) { return -ENOMEM; }
    }

    uint64_t missed = copy_from_user(pfd, (const void *)(uintptr_t)uptr, (uint64_t)n * sizeof(struct pollfd));
    if (missed > 0) { if (pfd != small) { kfree(pfd); } return -EFAULT; }
    for (unsigned i = 0; i < n; i++) { pfd[i].revents = 0; }

    int64_t r = poll_core(pfd, n, deadline_from_ms(tmo));
    if (r >= 0) {
        missed = copy_to_user((void *)(uintptr_t)uptr, pfd, (uint64_t)n * sizeof(struct pollfd));
        if (missed > 0) { r = -EFAULT; }
    }
    if (pfd != small) { kfree(pfd); }
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
    uint64_t missed;
    if (urd) { missed = copy_from_user(rd, (const void *)(uintptr_t)urd, sizeof rd);
               if (missed > 0) { return -EFAULT; } }
    if (uwr) { missed = copy_from_user(wr, (const void *)(uintptr_t)uwr, sizeof wr);
               if (missed > 0) { return -EFAULT; } }
    if (uex) { missed = copy_from_user(ex, (const void *)(uintptr_t)uex, sizeof ex);
               if (missed > 0) { return -EFAULT; } }

    // Collect the set bits into a pollfd array sized to what the caller
    // actually described. This used to be a fixed 16-entry array
    // with `n < 16` in the loop condition, which silently
    // dropped every interesting fd past the sixteenth -- no error, no
    // truncation flag, just a wrong answer (CS2.2). nfds is already
    // bounded by FD_SETSIZE above, so the allocation is bounded too.
    unsigned interesting = 0;
    for (int fd = 0; fd < nfds; fd++) {
        int w = fd / 64, b = fd % 64;
        if ((rd[w] | wr[w] | ex[w]) & (1ULL << b)) { interesting++; }
    }
    if (interesting == 0) { return 0; }

    struct pollfd *pfd = kmalloc(interesting * sizeof(struct pollfd));
    if (!pfd) { return -ENOMEM; }

    unsigned n = 0;
    for (int fd = 0; fd < nfds && n < interesting; fd++) {
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
        long tv[2];
        uint64_t m = copy_from_user(tv, (const void *)(uintptr_t)utv, sizeof tv);
        if (m > 0) { kfree(pfd); return -EFAULT; }
        timeout_ms = (int)(tv[0] * 1000 + tv[1] / 1000);
        if (timeout_ms < 0) { timeout_ms = 0; }
    }

    int64_t r = poll_core(pfd, n, deadline_from_ms(timeout_ms));
    if (r < 0) { kfree(pfd); return r; }

    // Rebuild the sets from revents.
    for (int i = 0; i < FD_WORDS; i++) { rd[i] = wr[i] = ex[i] = 0; }
    int64_t count = 0;
    for (unsigned i = 0; i < n; i++) {
        int fd = pfd[i].fd, w = fd / 64, b = fd % 64;
        if (pfd[i].revents & POLLIN)  { rd[w] |= (1ULL << b); count++; }
        if (pfd[i].revents & POLLOUT) { wr[w] |= (1ULL << b); count++; }
        if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) { ex[w] |= (1ULL << b); count++; }
    }
    int64_t out_rc = count;
    if (urd) { missed = copy_to_user((void *)(uintptr_t)urd, rd, sizeof rd); if (missed > 0) { out_rc = -EFAULT; } }
    if (uwr) { missed = copy_to_user((void *)(uintptr_t)uwr, wr, sizeof wr); if (missed > 0) { out_rc = -EFAULT; } }
    if (uex) { missed = copy_to_user((void *)(uintptr_t)uex, ex, sizeof ex); if (missed > 0) { out_rc = -EFAULT; } }
    kfree(pfd);
    return out_rc;
}

// ---- epoll -------------------------------------------------------------
//
// See kernel/sync/epoll.h/.c for the object itself. epoll_wait() below
// is the only piece that belongs here rather than there: it is the one
// place an epoll object's registration list meets poll_core, the exact
// scan-and-sleep loop poll()/select() already use above.

// struct epoll_event -- Linux's x86_64 ABI shape exactly: packed, so
// the union (8 bytes) sits immediately after events (4 bytes) with no
// padding, unlike this struct's natural alignment. Musl's own
// <sys/epoll.h> declares the same layout with the same
// __attribute__((packed)) for the same reason.
struct epoll_event_abi {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

int64_t sys_epoll_create1(struct syscall_args *a) {
    return epoll_create((int)a->a1);
}

int64_t sys_epoll_ctl(struct syscall_args *a) {
    int epfd = (int)a->a1;
    int op   = (int)a->a2;
    int fd   = (int)a->a3;
    uint64_t uevent = a->a4;

    struct process *p = current_proc();
    struct file_descriptor *epf = fd_get(p, epfd);
    if (!epf) { return -EBADF; }

    uint32_t events = 0;
    uint64_t data = 0;
    // EPOLL_CTL_DEL is the one op Linux lets pass event as NULL for --
    // it is unused there (there is nothing left to update).
    if (uevent) {
        struct epoll_event_abi ev;
        uint64_t missed = copy_from_user(&ev, (const void *)(uintptr_t)uevent, sizeof ev);
        if (missed > 0) { return -EFAULT; }
        events = ev.events;
        data = ev.data;
    }
    return epoll_ctl_do(epf, op, fd, events, data);
}

static int64_t epoll_wait_core(struct syscall_args *a) {
    int epfd      = (int)a->a1;
    uint64_t uev  = a->a2;
    int maxevents = (int)a->a3;
    int timeout_ms = (int)a->a4;

    if (maxevents <= 0) { return -EINVAL; }
    if (!uev) { return -EFAULT; }

    struct process *p = current_proc();
    struct file_descriptor *epf = fd_get(p, epfd);
    if (!epf || epf->ops != &epoll_file_ops) { return -EBADF; }
    struct epoll_obj *o = (struct epoll_obj *)epf->priv;

    // Snapshot the registration list into a pollfd[] array under the
    // object's own lock, then run poll_core with the lock released --
    // poll_core can sleep, and nothing may hold a lock across that.
    uint64_t flags = spin_lock_irqsave(&o->lock);
    int n = o->nfds;
    if (n > maxevents) { n = maxevents; }
    struct pollfd *pfd = 0;
    uint64_t *edata = 0;
    if (n > 0) {
        pfd = kmalloc((uint64_t)n * sizeof(*pfd));
        edata = kmalloc((uint64_t)n * sizeof(*edata));
        if (!pfd || !edata) {
            spin_unlock_irqrestore(&o->lock, flags);
            if (pfd) { kfree(pfd); }
            if (edata) { kfree(edata); }
            return -ENOMEM;
        }
        int i = 0;
        for (struct epoll_entry *e = o->list; e && i < n; e = e->next, i++) {
            pfd[i].fd = e->fd;
            pfd[i].events = (short)e->events;
            pfd[i].revents = 0;
            edata[i] = e->data;
        }
        n = i;   // the list may be shorter than nfds if it changed concurrently
    }
    spin_unlock_irqrestore(&o->lock, flags);

    if (n == 0) {
        // Nothing registered: still honour the timeout instead of
        // returning immediately, matching Linux's epoll_wait on an
        // empty set.
        if (timeout_ms != 0) {
            struct thread *self = current_thread();
            waitq_poll_enter();
            waitq_poll_wait((uint64_t)deadline_from_ms(timeout_ms), &self->poll_notified);
            waitq_poll_leave();
        }
        return 0;
    }

    int64_t ready = poll_core(pfd, (unsigned)n, deadline_from_ms(timeout_ms));
    if (ready > 0) {
        struct epoll_event_abi *out = kmalloc((uint64_t)ready * sizeof(*out));
        if (!out) { kfree(pfd); kfree(edata); return -ENOMEM; }
        int64_t j = 0;
        for (int i = 0; i < n && j < ready; i++) {
            if (pfd[i].revents) {
                out[j].events = (uint32_t)pfd[i].revents;
                out[j].data = edata[i];
                j++;
            }
        }
        uint64_t missed = copy_to_user((void *)(uintptr_t)uev, out, (uint64_t)ready * sizeof(*out));
        kfree(out);
        if (missed > 0) { ready = -EFAULT; }
    }
    kfree(pfd);
    kfree(edata);
    return ready;
}

int64_t sys_epoll_wait(struct syscall_args *a) {
    return epoll_wait_core(a);
}

// epoll_pwait(epfd, events, maxevents, timeout, sigmask) -- the sigmask
// (a5, frame->r8) is not applied: NeoOS has no per-call signal-mask
// swap for any blocking syscall yet (see docs/stdlib.md's
// rt_sigsuspend entry for the one place that need is met today).
// Every real caller in this milestone passes a null sigmask; recorded
// as a divergence rather than silently ignored.
int64_t sys_epoll_pwait(struct syscall_args *a) {
    return epoll_wait_core(a);
}

// ---- inotify -------------------------------------------------------
//
// See kernel/sync/inotify.h/.c for the object and its scope (a real
// fd, correctly never-readable, no actual filesystem watching).

int64_t sys_inotify_init1(struct syscall_args *a) {
    return inotify_create((int)a->a1);
}

// a2/a3 are the watched path's (pointer, length) -- unused: the watch
// is real (a distinct wd comes back) but names nothing, since nothing
// will ever fire on it either way. a4 is the requested mask.
int64_t sys_inotify_add_watch(struct syscall_args *a) {
    struct process *p = current_proc();
    struct file_descriptor *f = fd_get(p, (int)a->a1);
    if (!f) { return -EBADF; }
    return inotify_add_watch_do(f, (uint32_t)a->a4);
}

int64_t sys_inotify_rm_watch(struct syscall_args *a) {
    struct process *p = current_proc();
    struct file_descriptor *f = fd_get(p, (int)a->a1);
    if (!f) { return -EBADF; }
    return inotify_rm_watch_do(f, (int)a->a2);
}
