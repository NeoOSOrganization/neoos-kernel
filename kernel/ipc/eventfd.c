// kernel/ipc/eventfd.c -- eventfd2(2).
//
// A 64-bit counter behind a file descriptor. Modelled on kernel/ipc/
// pipe.c: one lock, one waitq for blocked readers (counter == 0), one
// for blocked writers (an add would overflow), and one poll_head so
// poll_core registers on the object rather than the global broadcast.

#include "ipc/eventfd.h"
#include "fs/file.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "sync/poll_head.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "mm/heap.h"
#include "drivers/char/serial.h"
#include "errno.h"

#define EVENTFD_MAX 0xfffffffffffffffeULL   // Linux's ceiling (ULLONG_MAX-1)

struct eventfd {
    struct spinlock  lock;
    struct waitq     readers;
    struct waitq     writers;
    struct poll_head poll;
    uint64_t         count;
    int              semaphore;   // EFD_SEMAPHORE
    int              refs;
};

static struct eventfd *eventfd_alloc(unsigned int initval, int semaphore) {
    struct eventfd *e = (struct eventfd *)kmalloc(sizeof(*e));
    if (!e) { return 0; }
    for (unsigned i = 0; i < sizeof(*e); i++) { ((uint8_t *)e)[i] = 0; }
    spin_init(&e->lock, LOCK_RANK_EVENTFD, "eventfd");
    waitq_init(&e->readers);
    waitq_init(&e->writers);
    poll_head_init(&e->poll, "eventfd-poll");
    e->count     = initval;
    e->semaphore = semaphore ? 1 : 0;
    e->refs      = 1;
    return e;
}

// Unlocked core, so the selftest can drive it without an fd.
static int64_t eventfd_do_read(struct eventfd *e, int nonblock, uint64_t *out) {
    uint64_t f = spin_lock_irqsave(&e->lock);
    for (;;) {
        if (e->count > 0) { break; }
        if (nonblock) { spin_unlock_irqrestore(&e->lock, f); return -EAGAIN; }
        int rc = waitq_sleep(&e->readers, &e->lock);
        if (rc == -EINTR) { spin_unlock_irqrestore(&e->lock, f); return -EINTR; }
    }
    uint64_t v;
    if (e->semaphore) { v = 1; e->count -= 1; }
    else              { v = e->count; e->count = 0; }
    spin_unlock_irqrestore(&e->lock, f);

    waitq_wake_all(&e->writers);
    poll_head_notify(&e->poll);
    waitq_poll_notify();
    *out = v;
    return 8;
}

static int64_t eventfd_do_write(struct eventfd *e, int nonblock, uint64_t v) {
    if (v == 0xffffffffffffffffULL) { return -EINVAL; }
    uint64_t f = spin_lock_irqsave(&e->lock);
    for (;;) {
        if (e->count + v <= EVENTFD_MAX) { break; }
        if (nonblock) { spin_unlock_irqrestore(&e->lock, f); return -EAGAIN; }
        int rc = waitq_sleep(&e->writers, &e->lock);
        if (rc == -EINTR) { spin_unlock_irqrestore(&e->lock, f); return -EINTR; }
    }
    e->count += v;
    spin_unlock_irqrestore(&e->lock, f);

    waitq_wake_all(&e->readers);
    poll_head_notify(&e->poll);
    waitq_poll_notify();
    return 8;
}

// ---- file_ops --------------------------------------------------------

static int64_t evfd_read(struct file_descriptor *fd, void *buf, uint64_t len) {
    if (len < 8) { return -EINVAL; }
    uint64_t v = 0;
    int64_t rc = eventfd_do_read((struct eventfd *)fd->priv, fd->nonblock, &v);
    if (rc < 0) { return rc; }
    *(uint64_t *)buf = v;
    return 8;
}

static int64_t evfd_write(struct file_descriptor *fd, const void *buf, uint64_t len) {
    if (len < 8) { return -EINVAL; }
    return eventfd_do_write((struct eventfd *)fd->priv, fd->nonblock,
                            *(const uint64_t *)buf);
}

static int evfd_poll(struct file_descriptor *fd, int events) {
    struct eventfd *e = (struct eventfd *)fd->priv;
    if (!e) { return POLLERR; }
    int mask = 0;
    uint64_t f = spin_lock_irqsave(&e->lock);
    if (e->count > 0)           { mask |= POLLIN; }
    if (e->count < EVENTFD_MAX)  { mask |= POLLOUT; }
    spin_unlock_irqrestore(&e->lock, f);
    return mask & events;
}

static struct poll_head *evfd_poll_head(struct file_descriptor *fd) {
    struct eventfd *e = (struct eventfd *)fd->priv;
    return e ? &e->poll : 0;
}

static void evfd_dup(struct file_descriptor *fd) {
    struct eventfd *e = (struct eventfd *)fd->priv;
    if (!e) { return; }
    uint64_t f = spin_lock_irqsave(&e->lock);
    e->refs++;
    spin_unlock_irqrestore(&e->lock, f);
}

static void evfd_close(struct file_descriptor *fd) {
    struct eventfd *e = (struct eventfd *)fd->priv;
    fd->priv = 0;
    if (!e) { return; }
    uint64_t f = spin_lock_irqsave(&e->lock);
    int last = (--e->refs == 0);
    spin_unlock_irqrestore(&e->lock, f);
    if (last) { kfree(e); }
}

static const struct file_ops eventfd_ops = {
    .name      = "eventfd",
    .read      = evfd_read,
    .write     = evfd_write,
    .poll      = evfd_poll,
    .poll_head = evfd_poll_head,
    .dup       = evfd_dup,
    .close     = evfd_close,
};

int eventfd_create(unsigned int initval, int flags) {
    struct process *proc = current_proc();
    if (!proc) { return -ESRCH; }
    if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK)) { return -EINVAL; }

    struct eventfd *e = eventfd_alloc(initval, flags & EFD_SEMAPHORE);
    if (!e) { return -ENOMEM; }

    int fd = fd_table_alloc(proc->fd_table);
    if (fd < 0) { kfree(e); return fd; }

    struct file_descriptor *f = fd_table_get(proc->fd_table, fd);
    if (!f) { fd_table_close(proc->fd_table, fd); kfree(e); return -EBADF; }

    f->ops      = &eventfd_ops;
    f->priv     = e;
    f->readable = 1;
    f->writable = 1;
    f->nonblock = (flags & EFD_NONBLOCK) ? 1 : 0;
    // EFD_CLOEXEC accepted but inert -- NeoOS has no close-on-exec
    // machinery yet (same as pipe2's O_CLOEXEC). Recorded in docs/stdlib.md.
    return fd;
}

// ---- selftest -------------------------------------------------------

static void fail(const char *why) {
    serial_write_string("[eventfd] selftest FAILED: ");
    serial_write_string(why);
    serial_write_string("\n");
}

void eventfd_selftest(void) {
    struct eventfd *e = eventfd_alloc(0, 0);
    if (!e) { fail("alloc"); return; }
    uint64_t v = 0;

    if (eventfd_do_read(e, 1, &v) != -EAGAIN) { fail("empty read not EAGAIN"); kfree(e); return; }
    if (eventfd_do_write(e, 1, 5) != 8)       { fail("write"); kfree(e); return; }
    if (eventfd_do_write(e, 1, 3) != 8)       { fail("write 2"); kfree(e); return; }
    if (eventfd_do_read(e, 1, &v) != 8 || v != 8) { fail("read != sum"); kfree(e); return; }
    if (eventfd_do_read(e, 1, &v) != -EAGAIN) { fail("drained read not EAGAIN"); kfree(e); return; }
    if (eventfd_do_write(e, 1, 0xffffffffffffffffULL) != -EINVAL) { fail("UINT64_MAX write not EINVAL"); kfree(e); return; }
    if (eventfd_do_write(e, 1, EVENTFD_MAX) != 8) { fail("max write"); kfree(e); return; }
    if (eventfd_do_write(e, 1, 1) != -EAGAIN) { fail("overflow write not EAGAIN"); kfree(e); return; }
    kfree(e);

    // semaphore mode
    struct eventfd *s = eventfd_alloc(2, 1);
    if (!s) { fail("sem alloc"); return; }
    if (eventfd_do_read(s, 1, &v) != 8 || v != 1) { fail("sem read != 1"); kfree(s); return; }
    if (eventfd_do_read(s, 1, &v) != 8 || v != 1) { fail("sem read 2 != 1"); kfree(s); return; }
    if (eventfd_do_read(s, 1, &v) != -EAGAIN) { fail("sem drained not EAGAIN"); kfree(s); return; }
    kfree(s);

    serial_write_string("[eventfd] selftest passed\n");
}
