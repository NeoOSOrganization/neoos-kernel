// kernel/pipe.c -- POSIX pipes.
//
// A ring buffer, a lock, and two wait queues. The interesting parts are
// not the buffer; they are the three end-of-stream rules that every
// program relies on and that are easy to get subtly wrong:
//
//   1. A read from an empty pipe whose write ends are ALL closed
//      returns 0 (end of file), rather than blocking forever.
//   2. A write to a pipe whose read ends are ALL closed raises SIGPIPE
//      and returns -EPIPE, rather than filling a buffer nobody will
//      ever drain.
//   3. A blocked reader must be woken when the last writer closes, not
//      only when data arrives -- otherwise rule 1 is unreachable from
//      the blocked state, which is the state it exists for.
//
// Those three are why a pipe counts its readers and writers separately
// from its total reference count.

#include "ipc/pipe.h"
#include "fs/file.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "errno.h"
#include "dev/serial.h"
#include "ipc/signal.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "mm/heap.h"

// Linux's values, so a program compiled against Linux headers passes
// the right bits. Only O_NONBLOCK changes behaviour here.
#define PIPE_O_NONBLOCK 0x800
#define PIPE_O_CLOEXEC  0x80000

struct pipe {
    struct spinlock lock;
    struct waitq    readers;    // blocked waiting for data
    struct waitq    writers;    // blocked waiting for space

    uint8_t *buf;               // PIPE_CAPACITY bytes
    uint32_t head;              // next byte to read
    uint32_t count;             // bytes currently buffered

    // Counted separately from `refs` because the end-of-stream rules
    // above are about ENDS, not references: a pipe with two read fds
    // and no write fd is at EOF even though two references remain.
    int readers_open;
    int writers_open;
    int refs;                   // total fds pointing here

};

static struct pipe *pipe_alloc(void) {
    struct pipe *p = (struct pipe *)kmalloc(sizeof(struct pipe));
    if (!p) { return 0; }
    for (unsigned i = 0; i < sizeof(*p); i++) { ((uint8_t *)p)[i] = 0; }

    p->buf = (uint8_t *)kmalloc(PIPE_CAPACITY);
    if (!p->buf) { kfree(p); return 0; }

    spin_init(&p->lock, LOCK_RANK_PIPE, "pipe");
    waitq_init(&p->readers);
    waitq_init(&p->writers);
    return p;
}

static void pipe_free(struct pipe *p) {
    kfree(p->buf);
    kfree(p);
}

// Unlocked. Caller holds p->lock.
static uint32_t ring_read(struct pipe *p, uint8_t *out, uint32_t want) {
    uint32_t n = want < p->count ? want : p->count;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = p->buf[(p->head + i) % PIPE_CAPACITY];
    }
    p->head = (p->head + n) % PIPE_CAPACITY;
    p->count -= n;
    return n;
}

// Unlocked. Caller holds p->lock.
static uint32_t ring_write(struct pipe *p, const uint8_t *in, uint32_t want) {
    uint32_t space = PIPE_CAPACITY - p->count;
    uint32_t n = want < space ? want : space;
    uint32_t tail = (p->head + p->count) % PIPE_CAPACITY;
    for (uint32_t i = 0; i < n; i++) {
        p->buf[(tail + i) % PIPE_CAPACITY] = in[i];
    }
    p->count += n;
    return n;
}

static int64_t pipe_read(struct file_descriptor *f, void *buf, uint64_t len) {
    struct pipe *p = (struct pipe *)f->priv;
    if (!p) { return -EBADF; }
    if (len == 0) { return 0; }

    uint64_t flags = spin_lock_irqsave(&p->lock);
    for (;;) {
        if (p->count > 0) { break; }
        // RULE 1: no data and no writer left is end of file, not a
        // wait. Checked while holding the lock a closing writer must
        // also take, so the two cannot interleave into a lost EOF.
        if (p->writers_open == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return 0;
        }
        if (f->nonblock) {
            spin_unlock_irqrestore(&p->lock, flags);
            return -EAGAIN;
        }
        // Hands the lock to waitq_sleep, which enqueues under the wait
        // queue's own lock and only then releases this one -- so a
        // writer cannot slip data in and wake an empty queue between
        // the check above and the sleep.
        int rc = waitq_sleep(&p->readers, &p->lock);
        if (rc == -EINTR) {
            spin_unlock_irqrestore(&p->lock, flags);
            return -EINTR;
        }
    }

    uint32_t n = ring_read(p, (uint8_t *)buf, (uint32_t)len);
    spin_unlock_irqrestore(&p->lock, flags);

    // Woken outside the lock: a writer that becomes runnable here has
    // no reason to contend for a lock this thread is about to drop
    // anyway.
    waitq_wake_all(&p->writers);
    return (int64_t)n;
}

static int64_t pipe_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    struct pipe *p = (struct pipe *)f->priv;
    if (!p) { return -EBADF; }
    if (len == 0) { return 0; }

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t written = 0;

    uint64_t flags = spin_lock_irqsave(&p->lock);
    while (written < len) {
        // RULE 2: nobody will ever read this. POSIX says raise SIGPIPE
        // and fail with EPIPE -- and the signal comes first, because a
        // process with the default disposition dies here and never sees
        // the return value at all.
        if (p->readers_open == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            struct thread *t = current_thread();
            if (t && t->proc) {
                struct siginfo info;
                siginfo_user(&info, SIGPIPE, t->proc->pid);
                signal_send_thread(t, SIGPIPE, &info);
            }
            // A partial write that then hits a dead reader reports the
            // bytes that DID land, as POSIX requires; only a write that
            // transferred nothing reports the error.
            return written ? (int64_t)written : -EPIPE;
        }

        uint32_t n = ring_write(p, in + written, (uint32_t)(len - written));
        written += n;
        if (written == len) { break; }

        // Full. A non-blocking write reports what it managed.
        if (f->nonblock) {
            spin_unlock_irqrestore(&p->lock, flags);
            waitq_wake_all(&p->readers);
            return written ? (int64_t)written : -EAGAIN;
        }

        // Wake readers BEFORE sleeping: the bytes just written are what
        // will let one of them make room.
        spin_unlock_irqrestore(&p->lock, flags);
        waitq_wake_all(&p->readers);
        flags = spin_lock_irqsave(&p->lock);

        if (p->count == PIPE_CAPACITY) {
            int rc = waitq_sleep(&p->writers, &p->lock);
            if (rc == -EINTR) {
                spin_unlock_irqrestore(&p->lock, flags);
                return written ? (int64_t)written : -EINTR;
            }
        }
    }
    spin_unlock_irqrestore(&p->lock, flags);

    waitq_wake_all(&p->readers);
    return (int64_t)written;
}

static int64_t pipe_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -ESPIPE;   // POSIX's answer for a seek on a pipe
}

static int64_t pipe_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f; (void)buf; (void)bytes;
    return -ENOTDIR;
}

// Deliberately LOCK-FREE, unlike pipe_close.
//
// fd_table_dup copies a whole bucket under the fd bucket lock, which is
// rank FDTABLE (15) -- above the pipe lock (10). Taking p->lock there
// is a descending acquire and the rank checker panics on it, which is
// exactly what it caught on the first boot of this code.
//
// Doing it without the lock is sound because a dup only ever
// INCREMENTS, and every counter is touched with an atomic
// read-modify-write on both sides, so nothing is lost. The one thing a
// lockless increment could break -- a reader seeing writers_open == 0
// and reporting EOF while a writer is being added -- cannot happen:
// fork copies EXISTING open descriptors, so if the count had reached
// zero there was no write end left to copy.
static void pipe_dup(struct file_descriptor *f) {
    struct pipe *p = (struct pipe *)f->priv;
    if (!p) { return; }
    __atomic_fetch_add(&p->refs, 1, __ATOMIC_ACQ_REL);
    if (f->readable) { __atomic_fetch_add(&p->readers_open, 1, __ATOMIC_ACQ_REL); }
    if (f->writable) { __atomic_fetch_add(&p->writers_open, 1, __ATOMIC_ACQ_REL); }
}

static void pipe_close(struct file_descriptor *f) {
    struct pipe *p = (struct pipe *)f->priv;
    if (!p) { return; }
    f->priv = 0;

    // The DECREMENTS happen under p->lock even though they are atomic,
    // and that is the load-bearing part. A reader checks writers_open
    // and then sleeps without ever releasing p->lock in between; a
    // closer that decremented and woke without taking the lock could
    // slip between the check and the enqueue, and its wake would find
    // an empty queue while the reader slept on for ever.
    uint64_t flags = spin_lock_irqsave(&p->lock);
    if (f->readable) { __atomic_fetch_sub(&p->readers_open, 1, __ATOMIC_ACQ_REL); }
    if (f->writable) { __atomic_fetch_sub(&p->writers_open, 1, __ATOMIC_ACQ_REL); }
    int last = (__atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL) == 0);
    int no_writers = (__atomic_load_n(&p->writers_open, __ATOMIC_ACQUIRE) == 0);
    int no_readers = (__atomic_load_n(&p->readers_open, __ATOMIC_ACQUIRE) == 0);
    spin_unlock_irqrestore(&p->lock, flags);

    if (last) { pipe_free(p); return; }

    // RULE 3. A reader blocked on an empty pipe is waiting for data
    // that is now never coming; waking it is the only way it can reach
    // the EOF test. The same in reverse for a writer blocked on a full
    // pipe whose last reader just went away -- it has to wake to
    // discover it should take SIGPIPE.
    if (no_writers) { waitq_wake_all(&p->readers); }
    if (no_readers) { waitq_wake_all(&p->writers); }
}

static const struct file_ops pipe_ops = {
    .name     = "pipe",
    .read     = pipe_read,
    .write    = pipe_write,
    .lseek    = pipe_lseek,
    .getdents = pipe_getdents,
    .dup      = pipe_dup,
    .close    = pipe_close,
};

const struct file_ops *pipe_file_ops(void) { return &pipe_ops; }

int pipe_create(int fds[2], int flags) {
    struct process *proc = current_proc();
    if (!proc) { return -ESRCH; }
    if (flags & ~(PIPE_O_NONBLOCK | PIPE_O_CLOEXEC)) { return -EINVAL; }

    struct pipe *p = pipe_alloc();
    if (!p) { return -ENOMEM; }

    int rfd = fd_table_alloc(proc->fd_table);
    if (rfd < 0) { pipe_free(p); return rfd; }
    int wfd = fd_table_alloc(proc->fd_table);
    if (wfd < 0) { fd_table_close(proc->fd_table, rfd); pipe_free(p); return wfd; }

    struct file_descriptor *r = fd_table_get(proc->fd_table, rfd);
    struct file_descriptor *w = fd_table_get(proc->fd_table, wfd);
    if (!r || !w) {
        fd_table_close(proc->fd_table, rfd);
        fd_table_close(proc->fd_table, wfd);
        pipe_free(p);
        return -EBADF;
    }

    // Reference counts are set directly rather than through pipe_dup:
    // these are the pipe's first two ends, and there is nothing to
    // increment from.
    p->refs         = 2;
    p->readers_open = 1;
    p->writers_open = 1;

    int nb = (flags & PIPE_O_NONBLOCK) ? 1 : 0;
    r->ops = &pipe_ops; r->priv = p; r->readable = 1; r->writable = 0; r->nonblock = nb;
    w->ops = &pipe_ops; w->priv = p; w->readable = 0; w->writable = 1; w->nonblock = nb;

    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

// ------------------------------------------------------------- selftest

// Exercises the ring buffer and the reference-counting rules from the
// kernel side, where a failure is a named line rather than a hung
// userland suite. The blocking paths belong to the userland test, which
// has real concurrent threads to block against.
void pipe_selftest(void) {
    struct pipe *p = pipe_alloc();
    if (!p) {
        serial_write_string("[pipe] selftest FAILED: allocation\n");
        return;
    }

    // Wrap-around: fill, drain most of it, then write past the end of
    // the buffer so head and tail are on opposite sides of the seam.
    // A ring that only ever gets used linearly hides an off-by-one here
    // until a long-running program finds it.
    uint8_t src[PIPE_CAPACITY];
    for (uint32_t i = 0; i < PIPE_CAPACITY; i++) { src[i] = (uint8_t)(i & 0xFF); }

    if (ring_write(p, src, PIPE_CAPACITY) != PIPE_CAPACITY) {
        serial_write_string("[pipe] selftest FAILED: could not fill the ring\n");
        pipe_free(p);
        return;
    }
    if (ring_write(p, src, 1) != 0) {
        serial_write_string("[pipe] selftest FAILED: wrote past a full ring\n");
        pipe_free(p);
        return;
    }

    uint8_t dst[PIPE_CAPACITY];
    uint32_t drained = ring_read(p, dst, PIPE_CAPACITY - 100);
    if (drained != PIPE_CAPACITY - 100) {
        serial_write_string("[pipe] selftest FAILED: short drain\n");
        pipe_free(p);
        return;
    }
    for (uint32_t i = 0; i < drained; i++) {
        if (dst[i] != (uint8_t)(i & 0xFF)) {
            serial_write_string("[pipe] selftest FAILED: data corrupted on drain\n");
            pipe_free(p);
            return;
        }
    }

    // Now write across the seam and read everything back in order.
    if (ring_write(p, src, 200) != 200) {
        serial_write_string("[pipe] selftest FAILED: could not write across the wrap\n");
        pipe_free(p);
        return;
    }
    if (p->count != 300) {
        serial_write_string("[pipe] selftest FAILED: count wrong after wrap\n");
        pipe_free(p);
        return;
    }
    uint32_t n = ring_read(p, dst, 300);
    if (n != 300) {
        serial_write_string("[pipe] selftest FAILED: short read after wrap\n");
        pipe_free(p);
        return;
    }
    // The first 100 bytes are the tail of the original fill; the next
    // 200 are the fresh write.
    for (uint32_t i = 0; i < 100; i++) {
        if (dst[i] != (uint8_t)((PIPE_CAPACITY - 100 + i) & 0xFF)) {
            serial_write_string("[pipe] selftest FAILED: wrapped data out of order\n");
            pipe_free(p);
            return;
        }
    }
    for (uint32_t i = 0; i < 200; i++) {
        if (dst[100 + i] != (uint8_t)(i & 0xFF)) {
            serial_write_string("[pipe] selftest FAILED: post-wrap data corrupted\n");
            pipe_free(p);
            return;
        }
    }
    if (p->count != 0) {
        serial_write_string("[pipe] selftest FAILED: ring not empty after full drain\n");
        pipe_free(p);
        return;
    }

    pipe_free(p);
    serial_write_string("[pipe] selftest passed\n");
}
